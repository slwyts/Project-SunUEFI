from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / 'linux/desktops/gnome/ambient-policy'


class GsdAmbientProfileTest(unittest.TestCase):
    def test_real_policy_low_light_and_recovery(self):
        if not shutil.which('cc') or not shutil.which('pkg-config'):
            self.skipTest('C compiler and GLib development files are required')
        flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'],
                               capture_output=True, text=True)
        if flags.returncode:
            self.skipTest('GLib development files are required')
        with tempfile.TemporaryDirectory(prefix='gsd-ambient-') as folder:
            work = Path(folder)
            source = work / 'policy.c'
            source.write_text('''
#include <math.h>
#include "gsd-ambient-profile.c"

int main (int argc, char **argv)
{
        g_autoptr(GKeyFile) file = g_key_file_new ();
        GsdAmbientProfile *profile = g_new0 (GsdAmbientProfile, 1);
        guint wait = 0;
        gdouble low, high;

        g_assert_cmpint (argc, ==, 2);
        g_assert_true (g_key_file_load_from_file (file, argv[1], G_KEY_FILE_NONE, NULL));
        g_assert_true (load_brightness_curve (file, profile));
        g_assert_true (load_curve (file, "BrighteningLux", "BrighteningThresholdLux", &profile->bright));
        g_assert_true (load_curve (file, "DarkeningLux", "DarkeningThresholdLux", &profile->dark));
        g_assert_true (load_curve (file, "SmallBrighteningLux", "SmallBrighteningThresholdLux", &profile->small));
        g_assert_true (load_delay (file, "BrighteningDebounceMs", &profile->bright_ms));
        g_assert_true (load_delay (file, "DarkeningDebounceMs", &profile->dark_ms));
        g_assert_true (load_delay (file, "SmallBrighteningDebounceMs", &profile->small_ms));

        g_assert_true (gsd_ambient_profile_set_brightness (profile, 197, 60));
        low = gsd_ambient_profile_get_brightness (profile, 7);
        high = gsd_ambient_profile_get_brightness (profile, 300);
        /* Ordinary indoor dimming must not collapse to the minimum. */
        g_assert_cmpfloat (low, >, 20);
        g_assert_cmpfloat (low, <, 60);
        g_assert_cmpfloat (high, >, 60);
        g_assert_cmpfloat (high, <, 65);
        g_assert_cmpfloat_with_epsilon (gsd_ambient_profile_get_brightness (profile, 197), 60, 1e-9);

        g_assert_true (gsd_ambient_profile_set_brightness (profile, 7, 61));
        low = gsd_ambient_profile_get_brightness (profile, 0);
        g_assert_true (isfinite (low));
        g_assert_cmpfloat (low, >, 0);
        g_assert_cmpfloat (low, <, 61);
        /* Neither zero lux nor a zero UI percentage may poison normalization. */
        g_assert_true (gsd_ambient_profile_set_brightness (profile, 0, 0));
        g_assert_cmpfloat (gsd_ambient_profile_get_brightness (profile, 0), ==, 0);
        g_assert_cmpfloat (gsd_ambient_profile_get_brightness (profile, 7), >, 0);
        g_assert_true (gsd_ambient_profile_set_brightness (profile, 7, 0));
        g_assert_cmpfloat (gsd_ambient_profile_get_brightness (profile, 100), >, 0);
        g_assert_false (gsd_ambient_profile_set_brightness (profile, NAN, 50));
        g_assert_false (gsd_ambient_profile_set_brightness (profile, 7, INFINITY));
        g_assert_false (gsd_ambient_profile_set_brightness (profile, 7, -1));

        gsd_ambient_profile_reset (profile);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 7, 1000000, &wait), ==, 7);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 0, 2000000, &wait), ==, 7);
        g_assert_cmpuint (wait, ==, 1000);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 4, 2500000, &wait), ==, 7);
        g_assert_cmpuint (wait, ==, 0);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 0, 3000000, &wait), ==, 7);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 0, 4000000, &wait), ==, 0);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 100, 5000000, &wait), ==, 0);
        g_assert_cmpfloat (gsd_ambient_profile_update (profile, 100, 6000000, &wait), ==, 100);
        g_assert_cmpfloat (gsd_ambient_profile_get_brightness (profile, 100), >, 0);
        gsd_ambient_profile_free (profile);

        profile = g_new0 (GsdAmbientProfile, 1);
        g_key_file_set_string (file, "Ambient", "BrightnessNits", "3.5;0;");
        g_key_file_set_string (file, "Ambient", "BrightnessLux", "0;1;");
        g_assert_false (load_brightness_curve (file, profile));
        gsd_ambient_profile_free (profile);
        return 0;
}
''')
            binary = work / 'policy'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(POLICY), str(source), '-o', str(binary),
                            *shlex.split(flags.stdout), '-lm'], check=True)
            subprocess.run([str(binary), str(POLICY / 'piano.ini')], check=True)


if __name__ == '__main__':
    unittest.main()
