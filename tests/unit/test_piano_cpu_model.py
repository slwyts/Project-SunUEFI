"""Actual CPU metadata overlay and cpuinfo output; no device or kernel build."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compose_piano_dtb import read_fdt
import build_piano_runtime_helpers as runtime
import build_release_helpers as helpers

MODEL = 'Qualcomm Snapdragon 8 Elite Mobile Platform (SM8750)'
PATCH = ROOT / 'patches/linux/7.2.9/0001-arm64-cpuinfo-read-dt-model.patch'
SOURCE = ROOT / 'upstream/linux-piano/arch/arm64/kernel/cpuinfo.c'
DTC = ROOT / 'build/kernel-topics/piano-panel/scripts/dtc/dtc'
FDTO = DTC.with_name('fdtoverlay')


class PianoCpuModelTests(unittest.TestCase):
    def test_generated_stable_uapi_keeps_helper_interfaces(self):
        old = ROOT / 'build/piano-runtime/uapi/include'
        new = ROOT / 'build/piano-runtime/uapi-7.2.9/include'
        candidate = ROOT / 'build/kernel-worktrees/release-7.2.9'
        if not old.exists() or not new.exists() or not candidate.exists():
            self.skipTest('Actual reviewed UAPI generations/candidate unavailable')
        before, after = runtime.tree_files(old), runtime.tree_files(new)
        changed = {name for name in before.keys() | after.keys() if before.get(name) != after.get(name)}
        self.assertEqual(changed, {'drm/amdgpu_drm.h', 'rdma/bnxt_re-abi.h', 'linux/version.h'})
        commit = subprocess.check_output(['git', '-C', str(candidate), 'rev-parse', 'HEAD'], text=True).strip()
        self.assertEqual(helpers.expected_uapi_digest(candidate, commit), runtime.tree_digest(after))
        self.assertEqual(helpers.expected_uapi_digest(ROOT / 'upstream/linux-piano', runtime.UAPI_COMMIT), runtime.tree_digest(before))

    @unittest.skipUnless(SOURCE.is_file() and shutil.which('cc'), 'CPU source/native compiler unavailable')
    def test_actual_cpuinfo_patch_model_and_original_fallbacks(self):
        with tempfile.TemporaryDirectory(prefix='piano-cpuinfo-') as temporary:
            folder = Path(temporary)
            source = folder / 'arch/arm64/kernel/cpuinfo.c'
            source.parent.mkdir(parents=True)
            source.write_bytes(SOURCE.read_bytes())
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(PATCH)], cwd=folder,
                           check=True, capture_output=True)
            text = source.read_text()
            helper = text[text.index('static void cpuinfo_show_model('):text.index('static int c_show(')]
            prepared = ROOT / 'build/kernel-worktrees/release-7.2.9/arch/arm64/kernel/cpuinfo.c'
            if prepared.exists():
                self.assertEqual(prepared.read_bytes(), source.read_bytes())
            harness = folder / 'cpuinfo.c'
            harness.write_text('''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
typedef uint32_t u32;
struct seq_file { char text[256]; };
struct device_node { const char *model; };
static struct device_node node;
static bool present;
static unsigned int released;
#define MIDR_REVISION(value) ((value) & 15)
#define COMPAT_ELF_PLATFORM "v8l"
static struct device_node *of_get_cpu_node(int cpu, void *unused)
{ (void)unused; if (cpu != 3) exit(3); return present ? &node : NULL; }
static int of_property_read_string(struct device_node *cpu, const char *key, const char **value)
{ if (strcmp(key, "model") || !cpu->model) return -1; *value = cpu->model; return 0; }
static void of_node_put(struct device_node *cpu) { if (cpu) released++; }
static void seq_printf(struct seq_file *m, const char *format, ...)
{ va_list args; va_start(args, format); vsnprintf(m->text, sizeof(m->text), format, args); va_end(args); }
''' + helper + '''
int main(int argc, char **argv)
{
    struct seq_file m = { 0 };
    if (argc != 3) return 2;
    present = strcmp(argv[1], "no-node") != 0;
    node.model = strcmp(argv[1], "no-node") && strcmp(argv[1], "no-model") ? argv[1] : NULL;
    cpuinfo_show_model(&m, 3, 7, atoi(argv[2]) != 0);
    fputs(m.text, stdout);
    return released != (present ? 1U : 0U);
}
''')
            binary = folder / 'cpuinfo'
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(harness), '-o', str(binary)], check=True)
            for name, compat, expected in (
                (MODEL, '0', 'model name\t: ' + MODEL + '\n'),
                (MODEL, '1', 'model name\t: ' + MODEL + '\n'),
                ('no-model', '0', ''), ('no-node', '0', ''),
                ('no-model', '1', 'model name\t: ARMv8 Processor rev 7 (v8l)\n'),
                ('no-node', '1', 'model name\t: ARMv8 Processor rev 7 (v8l)\n'),
            ):
                with self.subTest(name=name, compat=compat):
                    result = subprocess.run([str(binary), name, compat], check=True, capture_output=True, text=True)
                    self.assertEqual(result.stdout, expected)

    @unittest.skipUnless(DTC.exists() and FDTO.exists(), 'Real DT compiler/overlay tools required')
    def test_overlay_changes_only_eight_real_cpu_model_properties(self):
        base = ROOT / 'vendor/piano-linux/board.dtb'
        if not base.exists():
            self.skipTest('Captured board DTB unavailable')
        original = base.read_bytes()
        before = read_fdt(original)
        with tempfile.TemporaryDirectory(prefix='piano-cpu-model-') as temporary:
            folder = Path(temporary)
            overlay, final = folder / 'cpu-model.dtbo', folder / 'board.dtb'
            subprocess.run([str(DTC), '-@', '-I', 'dts', '-O', 'dtb', '-o', str(overlay),
                            str(ROOT / 'linux/dts/piano-cpu-model.dtso')], check=True)
            subprocess.run([str(FDTO), '-i', str(base), '-o', str(final), str(overlay)], check=True)
            after = read_fdt(final.read_bytes())
        changed = {(path, name) for path in before['tree'].keys() | after['tree'].keys()
                   for name in before['tree'].get(path, {}).keys() | after['tree'].get(path, {}).keys()
                   if before['tree'].get(path, {}).get(name) != after['tree'].get(path, {}).get(name)}
        cpus = {path for path, props in before['tree'].items() if props.get('device_type') == b'cpu\0'}
        self.assertEqual(len(cpus), 8)
        self.assertEqual(changed, {(path, 'model') for path in cpus})
        for path in cpus:
            self.assertEqual(after['tree'][path]['model'], MODEL.encode() + b'\0')
        self.assertEqual(before['reservations'], after['reservations'])
        self.assertEqual(base.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
