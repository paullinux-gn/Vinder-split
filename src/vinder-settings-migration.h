/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <gio/gio.h>

/* Copies the settings that older Vinder versions stored in the location shared
 * with GNOME Files (/org/gnome/nautilus/...) into Vinder's own location.
 *
 * Runs once: it is skipped when the "legacy-settings-migrated" key of
 * @preferences is already set. Only values the user actually changed are
 * copied, so defaults keep following the schema. */
void vinder_settings_migrate_legacy (GSettings *preferences,
                                     GSettings *compression,
                                     GSettings *icon_view,
                                     GSettings *list_view,
                                     GSettings *window_state);
