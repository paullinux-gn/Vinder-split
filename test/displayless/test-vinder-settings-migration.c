/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <gio/gio.h>

#include "vinder-settings-migration.h"

#define VINDER_ID "io.github.paullinux_gn.Vinder."
#define LEGACY_ID VINDER_ID "Legacy."

static GSettings *
new_legacy (const char *name)
{
    g_autofree char *schema_id = g_strconcat (LEGACY_ID, name, NULL);
    g_autofree char *path = g_strconcat ("/org/gnome/nautilus/", name, "/", NULL);
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (source, schema_id, FALSE);

    g_assert_nonnull (schema);

    return g_settings_new_full (schema, NULL, path);
}

static void
test_copies_changed_values_once (void)
{
    g_autoptr (GSettings) legacy_preferences = new_legacy ("preferences");
    g_autoptr (GSettings) legacy_list_view = new_legacy ("list-view");
    g_autoptr (GSettings) preferences = g_settings_new (VINDER_ID "preferences");
    g_autoptr (GSettings) compression = g_settings_new (VINDER_ID "compression");
    g_autoptr (GSettings) icon_view = g_settings_new (VINDER_ID "icon-view");
    g_autoptr (GSettings) list_view = g_settings_new (VINDER_ID "list-view");
    g_autoptr (GSettings) window_state = g_settings_new (VINDER_ID "window-state");
    g_autofree char *appearance = NULL;
    g_autofree char *zoom = NULL;

    /* Values changed in the old, shared location... */
    g_settings_set_string (legacy_preferences, "vinder-appearance", "dark");
    g_settings_set_boolean (legacy_preferences, "vinder-blur", TRUE);
    g_settings_set_string (legacy_list_view, "default-zoom-level", "large");

    g_assert_false (g_settings_get_boolean (preferences, "legacy-settings-migrated"));

    vinder_settings_migrate_legacy (preferences, compression, icon_view, list_view, window_state);

    /* ...are copied into Vinder's own location, once. */
    appearance = g_settings_get_string (preferences, "vinder-appearance");
    g_assert_cmpstr (appearance, ==, "dark");
    g_assert_true (g_settings_get_boolean (preferences, "vinder-blur"));
    zoom = g_settings_get_string (list_view, "default-zoom-level");
    g_assert_cmpstr (zoom, ==, "large");
    g_assert_true (g_settings_get_boolean (preferences, "legacy-settings-migrated"));

    /* Keys the user never changed keep following the schema default. */
    g_assert_true (g_settings_get_user_value (icon_view, "default-zoom-level") == NULL);

    /* Later changes in the old location (e.g. made in GNOME Files) are not copied again. */
    g_settings_set_string (legacy_preferences, "vinder-appearance", "dark-and-light");
    vinder_settings_migrate_legacy (preferences, compression, icon_view, list_view, window_state);
    g_clear_pointer (&appearance, g_free);
    appearance = g_settings_get_string (preferences, "vinder-appearance");
    g_assert_cmpstr (appearance, ==, "dark");

    /* And Vinder's own writes never touch the old location. */
    g_settings_set_string (preferences, "vinder-appearance", "light");
    g_clear_pointer (&appearance, g_free);
    appearance = g_settings_get_string (legacy_preferences, "vinder-appearance");
    g_assert_cmpstr (appearance, ==, "dark-and-light");
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/vinder-settings-migration/copies-changed-values-once",
                     test_copies_changed_values_once);

    return g_test_run ();
}
