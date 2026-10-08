/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional measured-lux policy in the existing power manager, not a sensor. */
#include "gsd-ambient-profile.h"
#include <math.h>
#include <string.h>

#ifndef GSD_AMBIENT_PROFILE_DIR
#define GSD_AMBIENT_PROFILE_DIR "/usr/share/gnome-settings-daemon/ambient-profiles"
#endif

typedef struct {
        gdouble *x, *y;
        gsize count;
} Curve;

struct _GsdAmbientProfile {
        Curve bright, dark, small;
        guint bright_ms, dark_ms, small_ms;
        gboolean initialized;
        gdouble accepted_lux;
        gint64 bright_since, dark_since, small_since;
};

static gboolean
compatible_matches (const gchar *wanted, const gchar *bytes, gsize size)
{
        const gchar *p = bytes, *end = bytes + size;
        while (p < end) {
                const gchar *zero = memchr (p, '\0', end - p);
                if (!zero)
                        return FALSE;
                if ((gsize) (zero - p) == strlen (wanted) && memcmp (p, wanted, zero - p) == 0)
                        return TRUE;
                p = zero + 1;
        }
        return FALSE;
}

static gboolean
load_curve (GKeyFile *file, const gchar *x_name, const gchar *y_name, Curve *curve)
{
        gsize yn = 0;
        curve->x = g_key_file_get_double_list (file, "Ambient", x_name, &curve->count, NULL);
        curve->y = g_key_file_get_double_list (file, "Ambient", y_name, &yn, NULL);
        if (!curve->x || !curve->y || curve->count < 2 || curve->count > 256 || yn != curve->count)
                return FALSE;
        for (gsize i = 0; i < curve->count; i++) {
                if (!isfinite (curve->x[i]) || !isfinite (curve->y[i]) ||
                    curve->x[i] < 0 || curve->y[i] < 0 ||
                    (i && curve->x[i] <= curve->x[i - 1]))
                        return FALSE;
        }
        return TRUE;
}

static gdouble
interpolate (const Curve *curve, gdouble lux)
{
        if (lux <= curve->x[0])
                return curve->y[0];
        for (gsize i = 1; i < curve->count; i++) {
                if (lux <= curve->x[i])
                        return curve->y[i - 1] + (curve->y[i] - curve->y[i - 1]) *
                                (lux - curve->x[i - 1]) / (curve->x[i] - curve->x[i - 1]);
        }
        return curve->y[curve->count - 1];
}

static gboolean
load_delay (GKeyFile *file, const gchar *name, guint *delay)
{
        g_autoptr(GError) error = NULL;
        guint64 value = g_key_file_get_uint64 (file, "Ambient", name, &error);
        if (error || !value || value > 60000)
                return FALSE;
        *delay = value;
        return TRUE;
}

void
gsd_ambient_profile_free (GsdAmbientProfile *profile)
{
        if (!profile)
                return;
        g_free (profile->bright.x); g_free (profile->bright.y);
        g_free (profile->dark.x); g_free (profile->dark.y);
        g_free (profile->small.x); g_free (profile->small.y);
        g_free (profile);
}

GsdAmbientProfile *
gsd_ambient_profile_load (void)
{
        g_autofree gchar *compatible = NULL;
        g_autoptr(GDir) dir = NULL;
        gsize bytes = 0;
        const gchar *name;
        if (!g_file_get_contents ("/sys/firmware/devicetree/base/compatible", &compatible, &bytes, NULL))
                return NULL;
        dir = g_dir_open (GSD_AMBIENT_PROFILE_DIR, 0, NULL);
        if (!dir)
                return NULL;
        while ((name = g_dir_read_name (dir))) {
                g_autofree gchar *path = NULL, *match = NULL;
                g_autoptr(GKeyFile) file = g_key_file_new ();
                GsdAmbientProfile *profile;
                if (!g_str_has_suffix (name, ".ini"))
                        continue;
                path = g_build_filename (GSD_AMBIENT_PROFILE_DIR, name, NULL);
                if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
                        continue;
                match = g_key_file_get_string (file, "Match", "Compatible", NULL);
                if (!match || !compatible_matches (match, compatible, bytes))
                        continue;
                profile = g_new0 (GsdAmbientProfile, 1);
                if (!load_curve (file, "BrighteningLux", "BrighteningThresholdLux", &profile->bright) ||
                    !load_curve (file, "DarkeningLux", "DarkeningThresholdLux", &profile->dark) ||
                    !load_delay (file, "BrighteningDebounceMs", &profile->bright_ms) ||
                    !load_delay (file, "DarkeningDebounceMs", &profile->dark_ms) ||
                    (g_key_file_has_key (file, "Ambient", "SmallBrighteningLux", NULL) &&
                     (!load_curve (file, "SmallBrighteningLux", "SmallBrighteningThresholdLux", &profile->small) ||
                      !load_delay (file, "SmallBrighteningDebounceMs", &profile->small_ms)))) {
                        g_warning ("Ignoring invalid ambient-light profile %s", path);
                        gsd_ambient_profile_free (profile);
                        return NULL;
                }
                g_debug ("Using ambient-light profile %s for %s", path, match);
                return profile;
        }
        return NULL;
}

void
gsd_ambient_profile_reset (GsdAmbientProfile *profile)
{
        if (profile) {
                profile->initialized = FALSE;
                profile->bright_since = profile->dark_since = profile->small_since = -1;
        }
}

/* Each threshold has its own continuous interval: crossing the large
 * threshold must not discard time already spent above the small threshold. */
static gboolean
threshold_elapsed (gboolean crossed, guint delay_ms, gint64 now_us,
                   gint64 *since, guint *next_update_ms)
{
        gint64 remaining;
        if (!crossed) {
                *since = -1;
                return FALSE;
        }
        if (*since < 0 || now_us < *since)
                *since = now_us;
        remaining = (gint64) delay_ms * 1000 - (now_us - *since);
        if (remaining <= 0)
                return TRUE;
        guint wait_ms = (guint) ((remaining + 999) / 1000);
        *next_update_ms = *next_update_ms ? MIN (*next_update_ms, wait_ms) : wait_ms;
        return FALSE;
}

gdouble
gsd_ambient_profile_update (GsdAmbientProfile *profile, gdouble lux,
                            gint64 now_us, guint *next_update_ms)
{
        gdouble bright, dark;
        gboolean big_ready, dark_ready, small_ready = FALSE;
        *next_update_ms = 0;
        if (!profile || !isfinite (lux) || lux < 0)
                return lux;
        if (!profile->initialized) {
                profile->initialized = TRUE;
                profile->accepted_lux = lux;
                profile->bright_since = profile->dark_since = profile->small_since = -1;
                return lux;
        }
        bright = interpolate (&profile->bright, profile->accepted_lux);
        dark = interpolate (&profile->dark, profile->accepted_lux);
        big_ready = threshold_elapsed (lux > bright && lux > profile->accepted_lux,
                                       profile->bright_ms, now_us,
                                       &profile->bright_since, next_update_ms);
        dark_ready = threshold_elapsed (lux < dark && lux < profile->accepted_lux,
                                        profile->dark_ms, now_us,
                                        &profile->dark_since, next_update_ms);
        if (profile->small.count)
                small_ready = threshold_elapsed (lux > interpolate (&profile->small, profile->accepted_lux) &&
                                                 lux > profile->accepted_lux,
                                                 profile->small_ms, now_us,
                                                 &profile->small_since, next_update_ms);
        if (big_ready || dark_ready || small_ready) {
                profile->accepted_lux = lux;
                profile->bright_since = profile->dark_since = profile->small_since = -1;
                *next_update_ms = 0;
        }
        return profile->accepted_lux;
}
