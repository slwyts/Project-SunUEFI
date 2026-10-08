/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <glib.h>

typedef struct _GsdAmbientProfile GsdAmbientProfile;
GsdAmbientProfile *gsd_ambient_profile_load (void);
void gsd_ambient_profile_free (GsdAmbientProfile *profile);
void gsd_ambient_profile_reset (GsdAmbientProfile *profile);
gdouble gsd_ambient_profile_update (GsdAmbientProfile *profile,
                                  gdouble measured_lux,
                                  gint64 now_us,
                                  guint *next_update_ms);
