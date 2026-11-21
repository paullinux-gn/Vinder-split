/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vinder-settings-migration.h"

#define LEGACY_SCHEMA_PREFIX "io.github.paullinux_gn.Vinder.Legacy."
#define LEGACY_PATH_PREFIX "/org/gnome/nautilus/"
#define MIGRATED_KEY "legacy-settings-migrated"

static void
migrate_group (GSettingsSchemaSource *source,
               const char            *name,
               GSettings             *target)
{
    g_autofree char *legacy_id = g_strconcat (LEGACY_SCHEMA_PREFIX, name, NULL);
    g_autofree char *legacy_path = g_strconcat (LEGACY_PATH_PREFIX, name, "/", NULL);
    g_autoptr (GSettingsSchema) legacy_schema = NULL;
    g_autoptr (GSettingsSchema) target_schema = NULL;
    g_autoptr (GSettings) legacy = NULL;
    g_auto (GStrv) keys = NULL;

    legacy_schema = g_settings_schema_source_lookup (source, legacy_id, FALSE);
    if (legacy_schema == NULL)
    {
        /* Legacy schema not installed: nothing to migrate. */
        return;
    }

    g_object_get (target, "settings-schema", &target_schema, NULL);
    legacy = g_settings_new_full (legacy_schema, NULL, legacy_path);
    keys = g_settings_schema_list_keys (legacy_schema);

    for (guint i = 0; keys[i] != NULL; i++)
    {
        g_autoptr (GVariant) user_value = NULL;

        if (!g_settings_schema_has_key (target_schema, keys[i]))
        {
            continue;
        }

        /* NULL unless the user changed this key, so untouched keys stay on the default. */
        user_value = g_settings_get_user_value (legacy, keys[i]);
        if (user_value == NULL)
        {
            continue;
        }

        if (!g_settings_set_value (target, keys[i], user_value))
        {
            g_warning ("Could not copy legacy setting '%s%s'", name, keys[i]);
        }
    }
}

void
vinder_settings_migrate_legacy (GSettings *preferences,
                                GSettings *compression,
                                GSettings *icon_view,
                                GSettings *list_view,
                                GSettings *window_state)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    GSettings *targets[] = { preferences, compression, icon_view, list_view, window_state };
    const char *names[] = { "preferences", "compression", "icon-view", "list-view", "window-state" };

    if (source == NULL ||
        g_settings_get_boolean (preferences, MIGRATED_KEY))
    {
        return;
    }

    for (guint i = 0; i < G_N_ELEMENTS (targets); i++)
    {
        migrate_group (source, names[i], targets[i]);
    }

    g_settings_set_boolean (preferences, MIGRATED_KEY, TRUE);
}
