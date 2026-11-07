"""P81C body gestures in the pen owner, offline tool and BLE profile, no device"""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PEN = ROOT / 'linux/userspace/pen'
PROFILE = ROOT / 'linux/bsp/common/usr/share/piano/pen/p81c-radio.json'
CALIBRATION = ROOT / 'linux/bsp/common/usr/share/piano/pen/calibration/piano-boe.ini'

EV_SYN, EV_KEY = 0, 1
BTN_TOUCH, BTN_STYLUS, BTN_STYLUS2, BTN_TOOL_PEN = 0x14a, 0x14b, 0x14c, 0x140
SYN = (EV_SYN, 0, 0)

# A packet socket keeps one report per read, as hidraw does
HARNESS = r'''
#include "piano-pen-owner.c"
#include <sys/socket.h>

struct piano_pen_core { int unused; };
static struct piano_pen_core core;
struct piano_pen_core *piano_pen_core_create(const char *i, int v,
    enum piano_pen_orientation o, uint64_t a, char *e, size_t n)
{ (void)i; (void)v; (void)o; (void)a; (void)e; (void)n; return &core; }
void piano_pen_core_destroy(struct piano_pen_core *c) { (void)c; }
void piano_pen_core_reset(struct piano_pen_core *c) { (void)c; }
int piano_pen_core_info(const struct piano_pen_core *c, struct piano_pen_info *i)
{
    (void)c;
    memset(i, 0, sizeof(*i));
    i->x_max = 213599; i->y_max = 319999; i->pressure_max = 16383;
    i->tilt_min = -60; i->tilt_max = 60;
    return 0;
}
int piano_pen_core_report5(struct piano_pen_core *c, const uint8_t *r, size_t b,
    uint64_t t, char *e, size_t n)
{ (void)c; (void)r; (void)b; (void)t; (void)e; (void)n; return 0; }
int piano_pen_core_process29(struct piano_pen_core *c, const uint8_t *r, size_t b,
    uint64_t t, struct piano_pen_output *o, char *e, size_t n)
{ (void)c; (void)r; (void)b; (void)t; (void)o; (void)e; (void)n; return 0; }

static struct piano_pen_owner *owner;
static int hid[2], events[2];

static void dump(const char *name)
{
    struct input_event ev;
    int first = 1;
    printf("{\"case\":\"%s\",\"events\":[", name);
    while (read(events[0], &ev, sizeof(ev)) == sizeof(ev)) {
        printf("%s[%u,%u,%d]", first ? "" : ",", ev.type, ev.code, ev.value);
        first = 0;
    }
    puts("]}");
}

static void report2(uint8_t value)
{
    uint8_t report[2] = { 2, value };
    if (write(hid[1], report, sizeof(report)) != sizeof(report))
        exit(2);
    piano_pen_owner_read_hid(owner, POLLIN);
}

static void range(int in)
{
    struct piano_pen_output out = { .valid = in ? PIANO_PEN_COORDINATES_VALID : 0 };
    if (piano_pen_owner_input_report(owner, &out, 3, in, 0, in ? 1000 : 0, in ? 1000 : 0))
        exit(3);
}

int main(void)
{
    owner = piano_pen_owner_create("unused", NULL, UINT64_C(100000000));
    if (!owner || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, hid) ||
        pipe(events) || fcntl(events[0], F_SETFL, O_NONBLOCK) ||
        fcntl(hid[0], F_SETFL, O_NONBLOCK))
        return 1;
    owner->hid_fd = hid[0];
    owner->input_fd = events[1];
    owner->area = (struct piano_pen_input_area){ 319999, 213599, 1339, 1310, 1 };

    range(1); dump("enter");
    report2(0x58); dump("tap_down");
    report2(0x00); dump("tap_up");
    report2(0x61); report2(0x00); report2(0x5b); report2(0x00); dump("slides");
    report2(0x6e); report2(0x00); dump("squeeze_click");
    report2(0x6e); report2(0x61); report2(0x00); dump("squeeze_replaced");
    report2(0x6e);
    owner->squeeze_started -= UINT64_C(700000000);
    piano_pen_owner_tick(owner); dump("squeeze_hold");
    report2(0x5b); report2(0x00); dump("hold_replaced");
    report2(0x58); range(0); dump("tap_leaves_range");
    report2(0x00); report2(0x58); report2(0x00); dump("tap_out_of_range");
    return 0;
}
'''


def unavailable():
    compiler = shutil.which('cc')
    if not compiler:
        return 'Host C compiler unavailable'
    probe = subprocess.run([compiler, '-fsyntax-only', '-x', 'c', '-'],
                           input='#include <linux/uinput.h>\n#include <linux/hidraw.h>\n',
                           text=True, capture_output=True)
    if probe.returncode:
        return 'Linux UAPI headers unavailable'
    if not shutil.which('c++') or not shutil.which('make'):
        return 'Host C++ toolchain unavailable'
    return None


def load_bluetooth():
    spec = importlib.util.spec_from_file_location('piano_pen_bluetooth', PEN / 'piano-pen-bluetooth.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PianoPenOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Skip each native test, not the class, so the runner counts them
        cls.reason = unavailable()
        if cls.reason:
            return
        cls.workspace = tempfile.TemporaryDirectory(prefix='pen-owner-')
        folder = Path(cls.workspace.name)
        subprocess.run(['make', '-s', '-C', str(PEN), 'OUT=' + str(folder / 'core')],
                       check=True, capture_output=True)
        cls.offline = folder / 'core/piano-pen-offline'
        harness = folder / 'owner-test.c'
        harness.write_text(HARNESS)
        binary = folder / 'owner-test'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(PEN),
                        str(harness), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, text=True,
                                capture_output=True, timeout=5)
        cls.cases = {row['case']: [tuple(event) for event in row['events']]
                     for row in map(json.loads, result.stdout.splitlines())}

    @classmethod
    def tearDownClass(cls):
        if not cls.reason:
            cls.workspace.cleanup()

    def native(self):
        if self.reason:
            self.skipTest(self.reason)

    def test_double_tap_is_the_second_tool_button_in_range(self):
        self.native()
        self.assertEqual(self.cases['tap_down'], [(EV_KEY, BTN_STYLUS2, 1), SYN])
        self.assertEqual(self.cases['tap_up'], [(EV_KEY, BTN_STYLUS2, 0), SYN])

    def test_double_tap_needs_the_pen_in_range(self):
        self.native()
        leave = self.cases['tap_leaves_range']
        self.assertEqual(leave[:2], [(EV_KEY, BTN_STYLUS2, 1), SYN])
        self.assertIn((EV_KEY, BTN_STYLUS2, 0), leave[2:])
        self.assertIn((EV_KEY, BTN_TOOL_PEN, 0), leave[2:])
        self.assertEqual(self.cases['tap_out_of_range'], [])

    def test_slides_produce_no_tool_input(self):
        self.native()
        self.assertEqual(self.cases['slides'], [])

    def test_squeeze_click_and_hold_stay_unchanged(self):
        self.native()
        self.assertEqual(self.cases['squeeze_click'],
                         [(EV_KEY, BTN_TOUCH, 1), SYN, (EV_KEY, BTN_TOUCH, 0), SYN])
        self.assertEqual(self.cases['squeeze_hold'], [(EV_KEY, BTN_STYLUS, 1), SYN])

    def test_a_new_gesture_ends_the_squeeze_without_a_click(self):
        self.native()
        self.assertEqual(self.cases['squeeze_replaced'], [])
        self.assertEqual(self.cases['hold_replaced'], [(EV_KEY, BTN_STYLUS, 0), SYN])

    def test_offline_tool_names_report2_values(self):
        self.native()
        folder = Path(self.workspace.name) / 'offline'
        folder.mkdir(exist_ok=True)
        rows = []
        for stamp, value in enumerate((0x61, 0x00, 0x5b, 0x00, 0x58, 0x00, 0x6e, 0x00, 0x4b), 1):
            (folder / f'{stamp}.bin').write_bytes(bytes((2, value)))
            rows.append(f'{stamp} report2 {stamp}.bin')
        (folder / 'events.tsv').write_text('\n'.join(rows) + '\n')
        result = subprocess.run([str(self.offline), '--ini', str(CALIBRATION),
                                 '--pressure-max-age-ns', '100000000',
                                 '--events', str(folder / 'events.tsv')],
                                check=True, text=True, capture_output=True)
        names = [row['gesture'] for row in map(json.loads, result.stdout.splitlines())
                 if row['kind'] == 'report2']
        self.assertEqual(names, ['slide_up', 'release', 'slide_down', 'release', 'double_tap',
                                 'release', 'squeeze', 'release', None])
        (folder / 'bad.bin').write_bytes(bytes((5, 0x61)))
        (folder / 'bad.tsv').write_text('1 report2 bad.bin\n')
        bad = subprocess.run([str(self.offline), '--ini', str(CALIBRATION),
                              '--pressure-max-age-ns', '100000000',
                              '--events', str(folder / 'bad.tsv')], text=True, capture_output=True)
        self.assertEqual(bad.returncode, 1)

    def test_profile_enables_body_slide(self):
        module = load_bluetooth()
        self.assertIs(module.profile(PROFILE)['slide'], True)
        broken = json.loads(PROFILE.read_text())
        broken['slide'] = 1
        with tempfile.NamedTemporaryFile('w', suffix='.json') as handle:
            json.dump(broken, handle)
            handle.flush()
            with self.assertRaises(ValueError):
                module.profile(Path(handle.name))


if __name__ == '__main__':
    unittest.main()
