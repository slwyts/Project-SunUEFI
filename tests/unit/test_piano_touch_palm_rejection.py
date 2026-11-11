"""Palm classification and evdev order of the patched touch reader, no device"""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_runtime_helpers as runtime

# Drives the real patched source; the pen owner stubs only satisfy the link
HARNESS = r'''
#undef main
struct piano_pen_owner *piano_pen_owner_create(const char *i, const char *h, uint64_t a)
{ (void)i; (void)h; (void)a; return NULL; }
void piano_pen_owner_destroy(struct piano_pen_owner *o) { (void)o; }
int piano_pen_owner_poll_fd(struct piano_pen_owner *o) { (void)o; return -1; }
void piano_pen_owner_read_hid(struct piano_pen_owner *o, short r) { (void)o; (void)r; }
void piano_pen_owner_tick(struct piano_pen_owner *o) { (void)o; }
int piano_pen_owner_record(struct piano_pen_owner *o, const uint8_t *f, size_t b,
    uint64_t t, uint16_t s, struct piano_pen_output *out)
{ (void)o; (void)f; (void)b; (void)t; (void)s; (void)out; return 0; }
void piano_pen_owner_stats(const struct piano_pen_owner *o, struct piano_pen_owner_stats *s)
{ (void)o; (void)s; }
int piano_pen_owner_enable_input(struct piano_pen_owner *o) { (void)o; return -1; }

static int events[2];

static void blob(int row, int col, int rows, int cols, int value)
{
    for (int r = row; r < row + rows; r++)
        for (int c = col; c < col + cols; c++)
            delta[r * st.cols + c] = value;
}

static void points(const char *name)
{
    struct point pts[MAX_POINTS];
    int n = find_points(pts);

    printf("{\"case\":\"%s\",\"points\":[", name);
    for (int i = 0; i < n; i++)
        printf("%s{\"major\":%.2f,\"minor\":%.2f,\"palm\":%d}", i ? "," : "",
               pts[i].major_mm, pts[i].minor_mm, pts[i].palm);
    puts("]}");
}

static void report(const char *name, int active)
{
    struct input_event ev;
    int first = 1;

    ui_report(active);
    printf("{\"case\":\"%s\",\"events\":[", name);
    while (read(events[0], &ev, sizeof(ev)) == sizeof(ev)) {
        printf("%s[%u,%u,%d]", first ? "" : ",", ev.type, ev.code, ev.value);
        first = 0;
        if (ev.type == EV_SYN)
            break;
    }
    puts("]}");
}

static int frame(void)
{
    struct point pts[MAX_POINTS];
    int n = find_points(pts);

    return track(pts, n);
}

int main(void)
{
    st.rows = 40;
    st.cols = 60;
    opt.threshold = 200;
    opt.mode = MODE_INPUT;

    memset(delta, 0, sizeof(delta));
    blob(19, 29, 3, 3, 600);
    delta[20 * 60 + 30] = 1500;
    points("fingertip");

    memset(delta, 0, sizeof(delta));
    blob(19, 27, 3, 8, 600);
    delta[20 * 60 + 28] = 1500;
    delta[20 * 60 + 33] = 1400;
    points("fingers_together");

    memset(delta, 0, sizeof(delta));
    blob(10, 10, 7, 12, 800);
    delta[12 * 60 + 12] = 1200;
    delta[13 * 60 + 19] = 1100;
    blob(30, 50, 3, 3, 900);
    points("palm_and_finger");

    if (pipe(events))
        return 1;
    ui.fd = events[1];
    memset(delta, 0, sizeof(delta));
    blob(19, 29, 3, 3, 600);
    delta[20 * 60 + 30] = 1500;
    report("finger_down", frame());
    blob(16, 25, 8, 10, 600);
    delta[20 * 60 + 30] = 1500;
    report("grows_to_palm", frame());
    memset(delta, 0, sizeof(delta));
    blob(19, 29, 3, 3, 600);
    delta[20 * 60 + 30] = 1500;
    report("shrinks_still_palm", frame());
    memset(delta, 0, sizeof(delta));
    report("palm_lifted", frame());
    blob(10, 10, 7, 12, 800);
    report("palm_down", frame());
    memset(delta, 0, sizeof(delta));
    report("palm_lifted_again", frame());
    blob(19, 29, 3, 3, 600);
    delta[20 * 60 + 30] = 1500;
    report("finger_again", frame());
    report("pen_frame", release_as_palms());
    return 0;
}
'''


def unavailable():
    compiler = shutil.which('cc')
    if not compiler:
        return 'Host C compiler unavailable'
    probe = subprocess.run([compiler, '-fsyntax-only', '-x', 'c', '-'],
                           input='#include <linux/uinput.h>\n', text=True,
                           capture_output=True)
    if probe.returncode:
        return 'Linux UAPI headers unavailable'
    if not (ROOT / 'upstream/debian-piano-current' /
            runtime.PUBLIC_SOURCES['piano-touch-view'][0]).exists():
        return 'Pinned source not fetched'
    return None


class PianoTouchPalmRejectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # skip per test, not per class, so the runner counts them
        cls.reason = unavailable()
        if cls.reason:
            return
        compiler = shutil.which('cc')
        public = ROOT / 'upstream/debian-piano-current'
        cls.workspace = tempfile.TemporaryDirectory(prefix='touch-palm-')
        folder = Path(cls.workspace.name)
        cls.source, cls.provenance = runtime.derive_touch_source(public, folder)
        harness = folder / 'palm-test.c'
        harness.write_text('#define main PianoTouchMain\n#include ' +
                           json.dumps(str(cls.source)) + '\n' + HARNESS)
        binary = folder / 'palm-test'
        subprocess.run([compiler, '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'linux/userspace/pen'), str(harness),
                        '-lm', '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, text=True,
                                capture_output=True, timeout=5)
        cls.cases = {row['case']: row for row in map(json.loads, result.stdout.splitlines())}

    @classmethod
    def tearDownClass(cls):
        if not cls.reason:
            cls.workspace.cleanup()

    def setUp(self):
        if self.reason:
            self.skipTest(self.reason)

    def events(self, case):
        return [tuple(row) for row in self.cases[case]['events']]

    def test_patch_is_part_of_the_derived_source(self):
        files = [row['file'] for row in self.provenance['patches']]
        self.assertEqual(files[-1], 'tools/patches/piano-touch-view-palm-rejection.patch')
        self.assertIn('MT_TOOL_PALM', self.source.read_text())

    def test_fingertip_is_a_finger_and_a_palm_area_is_one_palm(self):
        finger, = self.cases['fingertip']['points']
        self.assertFalse(finger['palm'])
        self.assertLess(finger['major'], 20)
        rows = self.cases['palm_and_finger']['points']
        self.assertEqual(sorted(row['palm'] for row in rows), [0, 1])
        palm = next(row for row in rows if row['palm'])
        self.assertGreater(palm['major'], 40)

    def test_fingertips_held_together_stay_two_fingers(self):
        rows = self.cases['fingers_together']['points']
        self.assertEqual([row['palm'] for row in rows], [0, 0])
        self.assertGreater(rows[0]['major'], 30)

    def test_tool_type_precedes_tracking_id_and_palm_never_touches(self):
        EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
        BTN_TOUCH, ABS_X = 0x14a, 0x00
        SLOT, MAJOR, TOOL, TRACK = 0x2f, 0x30, 0x37, 0x39
        down = self.events('finger_down')
        self.assertEqual(down[:3], [(EV_ABS, SLOT, 0), (EV_ABS, TOOL, 0), (EV_ABS, TRACK, 0)])
        self.assertIn((EV_KEY, BTN_TOUCH, 1), down)
        self.assertTrue(any(code == MAJOR and value > 0 for _, code, value in down))
        grown = self.events('grows_to_palm')
        self.assertEqual(grown[:2], [(EV_ABS, SLOT, 0), (EV_ABS, TOOL, 2)])
        self.assertIn((EV_KEY, BTN_TOUCH, 0), grown)
        self.assertNotIn(ABS_X, [code for kind, code, _ in grown if kind == EV_ABS])
        shrunk = self.events('shrinks_still_palm')
        self.assertNotIn(TOOL, [code for _, code, _ in shrunk])
        self.assertNotIn(EV_KEY, [kind for kind, _, _ in shrunk])
        self.assertEqual(self.events('palm_lifted'), [(EV_ABS, SLOT, 0), (EV_ABS, TRACK, -1), (EV_SYN, 0, 0)])
        palm = self.events('palm_down')
        self.assertEqual(palm[:3], [(EV_ABS, SLOT, 0), (EV_ABS, TOOL, 2), (EV_ABS, TRACK, 1)])
        self.assertNotIn(EV_KEY, [kind for kind, _, _ in palm])
        finger = self.events('finger_again')
        self.assertEqual(finger[:3], [(EV_ABS, SLOT, 0), (EV_ABS, TOOL, 0), (EV_ABS, TRACK, 2)])
        self.assertIn((EV_KEY, BTN_TOUCH, 1), finger)

    def test_contacts_down_at_a_pen_frame_are_lifted_as_palms(self):
        self.assertEqual(self.events('pen_frame'),
                         [(3, 0x2f, 0), (3, 0x37, 2), (3, 0x39, -1), (1, 0x14a, 0), (0, 0, 0)])


if __name__ == '__main__':
    unittest.main()
