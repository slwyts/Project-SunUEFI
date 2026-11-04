"""The GNOME power overlay must keep its three-mode tuned-ppd contract."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = ROOT / 'linux/desktops/gnome/power-overlay'

PROFILES = ('piano-power-saver', 'piano-balanced', 'piano-performance')
CPU = '/sys/devices/system/cpu/cpufreq/'
GPU = '/sys/class/devfreq/3d00000.gpu/'
BOOST = CPU + 'boost'


def parse_ini(text):
    sections = {}
    current = None
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#') or line.startswith(';'):
            continue
        if line.startswith('[') and line.endswith(']'):
            current = sections.setdefault(line[1:-1], {})
            continue
        if current is None:
            continue
        key, _, value = line.partition('=')
        current[key.strip()] = value.strip()
    return sections


def tuned_profile(name):
    path = OVERLAY / 'usr/lib/tuned/profiles' / name / 'tuned.conf'
    return parse_ini(path.read_text())['sysfs']


class TunedProfileTest(unittest.TestCase):
    def test_tuned_profiles_are_ordered(self):
        # GNOME's three power modes reach these profiles through tuned-ppd, so
        # the ordering is the contract: saver < balanced <= performance, the GPU
        # ceiling never drops in a faster profile, and only performance boosts.
        saver, balanced, performance = (tuned_profile(name) for name in PROFILES)
        policy0 = CPU + 'policy0/scaling_max_freq'
        policy6 = CPU + 'policy6/scaling_max_freq'
        self.assertLess(int(saver[policy0]), int(balanced[policy0]))
        self.assertLess(int(saver[policy6]), int(balanced[policy6]))
        self.assertLess(int(saver[GPU + 'max_freq']), int(balanced[GPU + 'max_freq']))
        self.assertLessEqual(int(balanced[policy0]), int(performance[policy0]))
        self.assertLessEqual(int(balanced[policy6]), int(performance[policy6]))
        self.assertEqual(balanced[GPU + 'max_freq'], performance[GPU + 'max_freq'])
        self.assertEqual(saver[BOOST], '0')
        self.assertEqual(balanced[BOOST], '0')
        self.assertEqual(performance[BOOST], '1')

    def test_tuned_profiles_leave_the_gpu_floor_alone(self):
        # Writing the devfreq floor before lowering the ceiling makes the driver
        # raise the floor to the ceiling and pins the GPU. Only the ceiling is
        # written, so the floor keeps its 160 MHz default.
        for name in PROFILES:
            self.assertNotIn('min_freq', tuned_profile(name), name)

    def test_ppd_mapping_points_at_shipped_profiles(self):
        mapping = parse_ini((OVERLAY / 'etc/tuned/piano-ppd.conf').read_text())
        self.assertEqual(mapping['profiles'], {
            'power-saver': 'piano-power-saver',
            'balanced': 'piano-balanced',
            'performance': 'piano-performance',
        })
        for name in mapping['profiles'].values():
            self.assertTrue((OVERLAY / 'usr/lib/tuned/profiles' / name / 'tuned.conf').is_file(), name)

    def test_drop_in_mounts_the_mapping_over_the_conffile(self):
        # tuned-ppd ships /etc/tuned/ppd.conf as a conffile; the mapping is
        # bind-mounted so package upgrades never see a modified conffile.
        drop_in = (OVERLAY / 'usr/lib/systemd/system/tuned-ppd.service.d/50-piano-ppd.conf').read_text()
        self.assertIn('BindReadOnlyPaths=/etc/tuned/piano-ppd.conf:/etc/tuned/ppd.conf', drop_in)
        self.assertTrue((OVERLAY / 'etc/tuned/piano-ppd.conf').is_file())


if __name__ == '__main__':
    unittest.main()
