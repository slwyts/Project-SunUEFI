"""Replay the owned THP reader on the host; no device or second FIFO reader."""
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_runtime_helpers as runtime

PEN = ROOT / 'linux/userspace/pen'
# Without pen options the reader never creates the pen owner; these only satisfy the link
PEN_OWNER_STUBS = r'''
#include "piano-pen-owner.h"
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
'''


def record(sequence, timestamp, frame_type=3, epoch=False):
    payload = bytearray(72)
    struct.pack_into('<I', payload, 8, 13)
    struct.pack_into('<I', payload, 16, 0xffffffff - 13)
    payload[24] = 1
    struct.pack_into('<HH', payload, 28, sequence & 0xffff, 7)
    payload[48:50] = bytes((2, 2))
    struct.pack_into('<H', payload, 52, 360)
    payload[54], payload[56] = 2, frame_type
    struct.pack_into('<hhhh', payload, 64, 1000, 1000, 1000, 1000)
    checksum = -sum(struct.unpack('<26H', payload[20:])) & 0xffff
    struct.pack_into('<H', payload, 4, checksum)
    struct.pack_into('<H', payload, 12, 0xffff - checksum)
    frame = bytes(257) + payload
    return struct.pack('<IHHQQHHI', 0x3150544e, 32, len(frame), sequence,
                       timestamp, checksum, 1 | (2 if epoch else 0), 13) + frame


class PianoTouchObservabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('Host C compiler unavailable')
        cls.workspace = tempfile.TemporaryDirectory(prefix='touch-replay-')
        cls.folder = Path(cls.workspace.name)
        public = ROOT / 'upstream/debian-piano-current'
        cls.source, cls.provenance = runtime.derive_touch_source(public, cls.folder)
        cls.binary = cls.folder / 'piano-touch-view'
        cls.stubs = cls.folder / 'pen-owner-stubs.c'
        cls.stubs.write_text(PEN_OWNER_STUBS)
        subprocess.run([compiler, '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(PEN),
                        str(cls.source), str(cls.stubs), '-lm', '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def replay(self, frames, diagnostics=True):
        capture = self.folder / 'capture.bin'
        capture.write_bytes(b''.join(frames))
        env = os.environ.copy()
        env['PIANO_THP_STREAM'] = str(capture)
        argv = [str(self.binary), 'points', '--seconds', '0', '--type', '3',
                '--reference', '1']
        if diagnostics:
            argv += ['--diagnostics', '5']
        result = subprocess.run(argv, env=env, check=True, text=True,
                                capture_output=True, timeout=5)
        reports = [json.loads(line) for line in result.stderr.splitlines()
                   if line.startswith('{')]
        return result, reports

    def test_144_completion_rate_is_distinct_from_firmware_360_scan_metadata(self):
        frames = [record(i, 1000000000 + round((i - 1) * 1000000000 / 144))
                  for i in range(1, 10)]
        _, reports = self.replay(frames)
        self.assertEqual(len(reports), 1)
        row = reports[0]
        self.assertTrue(row['final'])
        self.assertTrue(row['replay'])
        self.assertEqual(row['records'], 9)
        self.assertEqual(row['algorithm_frames'], 8)
        self.assertEqual(row['reference_frames'], 1)
        self.assertEqual(row['checksum_ok'], 9)
        self.assertEqual(row['types'], {'3': 9})
        self.assertAlmostEqual(row['source_completion_hz'], 144, places=3)
        self.assertEqual(row['firmware_scan_rate'], 360)
        self.assertEqual(row['source_timestamp_ns'], 1055555556)
        self.assertEqual(row['algorithm_sequence'], 9)
        self.assertGreater(row['algorithm_complete_ns'], 0)
        self.assertEqual(row['input_report_attempts'], 0)
        self.assertEqual(row['input_syn_success'], 0)
        self.assertEqual(row['read_age_samples'], 0)
        self.assertEqual(row['input_age_samples'], 0)
        self.assertEqual(row['sequence_gaps'], 0)
        self.assertEqual(row['firmware_drop_frame_no'], 7)

    def test_type_filter_sequence_gap_reorder_and_epoch_are_visible(self):
        _, reports = self.replay([record(1, 100), record(2, 200, 6),
                                  record(4, 400), record(3, 300),
                                  record(5, 500), record(6, 50, epoch=True)])
        row = reports[0]
        self.assertEqual(row['records'], 6)
        self.assertEqual(row['type_skips'], 1)
        # the epoch frame restarts type probing and the reference, so it is not decoded
        self.assertEqual(row['algorithm_frames'], 3)
        self.assertEqual(row['sequence_gaps'], 1)
        self.assertEqual(row['sequence_reorders'], 1)
        self.assertEqual(row['timestamp_reorders'], 1)
        self.assertEqual(row['epochs'], 1)
        self.assertEqual(row['types'], {'3': 5, '6': 1})

    def test_default_quiet_and_diagnostic_patch_preserves_pinned_checkout(self):
        frames = [record(1, 100), record(2, 200)]
        quiet, reports = self.replay(frames, diagnostics=False)
        observed, _ = self.replay(frames)
        self.assertFalse(reports)
        self.assertEqual(quiet.stdout, observed.stdout)
        original = ROOT / 'upstream/debian-piano-current' / runtime.PUBLIC_SOURCES['piano-touch-view'][0]
        self.assertEqual(runtime.sha(original), self.provenance['public_source_sha256'])
        self.assertNotEqual(self.provenance['effective_source_sha256'], runtime.sha(original))

    def test_syn_success_and_failed_write_count_without_uinput_device(self):
        driver = self.folder / 'syn-test.c'
        driver.write_text('#define main PianoReplayMain\n#include ' + json.dumps(str(self.source)) + r'''
#undef main
int main(void)
{
    int descriptors[2];
    struct input_event event;
    if (pipe(descriptors)) return 1;
    opt.diagnostics = 5;
    ui.fd = descriptors[1];
    diag.sequence = 27;
    diag.timestamp_ns = boot_ns();
    ui_report(0);
    if (read(descriptors[0], &event, sizeof(event)) != sizeof(event) ||
        event.type != EV_SYN || event.code != SYN_REPORT ||
        diag.input_attempts != 1 || diag.input_frames != 1 ||
        diag.input_sequence != 27 || diag.input_age_count != 1 ||
        diag.input_complete_ns < diag.timestamp_ns || st.ui_write_errors) return 2;
    close(descriptors[0]); close(descriptors[1]); ui.fd = -1;
    ui_report(0);
    return diag.input_attempts != 2 || diag.input_frames != 1 || st.ui_write_errors != 1;
}
''')
        binary = self.folder / 'syn-test'
        subprocess.run([shutil.which('cc'), '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(PEN),
                        str(driver), str(self.stubs), '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
