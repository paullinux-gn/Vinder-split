/*
 * SPDX-FileCopyrightText: 1999, 2000, 2001 Eazel, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Authors: Ramiro Estrugo <ramiro@eazel.com>
 */

#include <config.h>

#include "nautilus-global-preferences.h"
#include "vinder-settings-migration.h"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#undef G_SETTINGS_ENABLE_BACKEND

#include "nautilus-file-utilities.h"

#include <string.h>

GSettings *nautilus_preferences;
GSettings *nautilus_compression_preferences;
GSettings *nautilus_icon_view_preferences;
GSettings *nautilus_list_view_preferences;
GSettings *nautilus_window_state;
GSettings *gtk_filechooser_preferences;
GSettings *gnome_lockdown_preferences;
GSettings *gnome_interface_preferences;
GSettings *gnome_privacy_preferences;
GSettings *gnome_user_share_preferences = NULL;
GSettings *localsearch_preferences;

#define FILE_SHARING_SCHEMA_ID "org.gnome.desktop.file-sharing"
#define FILE_SHARING_SERVICE_PATH "/org/gnome/settings-daemon/plugins/sharing/gnome-user-share-webdav/"
#define FILE_SHARING_SERVICE_SCHEMA_ID "org.gnome.settings-daemon.plugins.sharing.service"
#define UPPER_MOUSE_LIMIT 14
/* Forward and back buttons on the mouse */
static gboolean mouse_extra_buttons = TRUE;
static guint mouse_forward_button = 9;
static guint mouse_back_button = 8;

#if VINDER_SPLIT_PROFILE
static GSettingsBackend *vinder_split_settings_backend;

static gboolean
migrate_split_position_setting (const char *filename)
{
    g_autoptr (GKeyFile) key_file = g_key_file_new ();
    g_autoptr (GError) error = NULL;
    g_autofree char *value = NULL;
    double fraction;

    if (!g_key_file_load_from_file (key_file, filename, G_KEY_FILE_KEEP_COMMENTS, &error) ||
        !g_key_file_has_key (key_file, "window-state", "vinder-split-position", NULL))
    {
        return TRUE;
    }

    value = g_key_file_get_value (key_file, "window-state", "vinder-split-position", NULL);
    if (value == NULL || strpbrk (value, ".eE") != NULL)
    {
        return TRUE;
    }

    /* Older builds stored the divider as a pixel offset. Convert it using the
     * saved window width so it remains useful when the window size changes. */
    gint old_position = g_key_file_get_integer (key_file, "window-state",
                                                "vinder-split-position", NULL);
    gint window_width = 0;
    g_autofree char *initial_size = g_key_file_get_value (key_file, "window-state",
                                                           "initial-size", NULL);
    if (initial_size != NULL)
    {
        g_autoptr (GVariant) size = g_variant_parse (G_VARIANT_TYPE ("(ii)"),
                                                    initial_size, NULL, NULL, NULL);
        if (size != NULL)
        {
            g_variant_get_child (size, 0, "i", &window_width);
        }
    }

    fraction = old_position > 0 && window_width > 0
               ? (double) old_position / window_width : 0.5;
    g_key_file_set_double (key_file, "window-state", "vinder-split-position",
                           CLAMP (fraction, 0.2, 0.8));

    if (!g_key_file_save_to_file (key_file, filename, &error))
    {
        g_warning ("Could not migrate Vinder-split divider setting in '%s': %s",
                   filename, error->message);
        return FALSE;
    }

    return TRUE;
}

static GSettings *
new_vinder_settings (const char *schema_id)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (source, schema_id, TRUE);

    if (schema == NULL)
    {
        g_error ("Required Vinder settings schema '%s' is unavailable", schema_id);
    }

    if (vinder_split_settings_backend == NULL)
    {
        g_autofree char *directory = g_build_filename (g_get_user_config_dir (),
                                                        "vinder-split", NULL);
        g_autofree char *filename = g_build_filename (directory, "settings.ini", NULL);

        if (g_mkdir_with_parents (directory, 0700) != 0)
        {
            g_warning ("Could not create Vinder-split settings directory '%s'; using in-memory settings",
                       directory);
            vinder_split_settings_backend = g_memory_settings_backend_new ();
        }
        else
        {
            if (migrate_split_position_setting (filename))
            {
                vinder_split_settings_backend = g_keyfile_settings_backend_new (filename,
                                                                                 "/io/github/paullinux_gn/Vinder/",
                                                                                 "vinder-split");
            }
            else
            {
                g_warning ("Using in-memory settings because the old divider value could not be migrated");
                vinder_split_settings_backend = g_memory_settings_backend_new ();
            }
        }
    }

    return g_settings_new_full (schema, vinder_split_settings_backend, NULL);
}
#else
static GSettings *
new_vinder_settings (const char *schema_id)
{
    return g_settings_new (schema_id);
}
#endif

static void
use_extra_mouse_buttons_changed (gpointer callback_data)
{
    mouse_extra_buttons = g_settings_get_boolean (nautilus_preferences, NAUTILUS_PREFERENCES_MOUSE_USE_EXTRA_BUTTONS);
}

static void
mouse_back_button_changed (gpointer callback_data)
{
    int new_back_button;

    new_back_button = g_settings_get_uint (nautilus_preferences, NAUTILUS_PREFERENCES_MOUSE_BACK_BUTTON);

    /* Bounds checking */
    if (new_back_button < 6 || new_back_button > UPPER_MOUSE_LIMIT)
    {
        return;
    }

    mouse_back_button = new_back_button;
}

static void
mouse_forward_button_changed (gpointer callback_data)
{
    int new_forward_button;

    new_forward_button = g_settings_get_uint (nautilus_preferences, NAUTILUS_PREFERENCES_MOUSE_FORWARD_BUTTON);

    /* Bounds checking */
    if (new_forward_button < 6 || new_forward_button > UPPER_MOUSE_LIMIT)
    {
        return;
    }

    mouse_forward_button = new_forward_button;
}

void
nautilus_global_preferences_init (void)
{
    static gboolean initialized = FALSE;

    if (initialized)
    {
        return;
    }

    initialized = TRUE;

    nautilus_preferences = new_vinder_settings ("io.github.paullinux_gn.Vinder.preferences");
    nautilus_compression_preferences = new_vinder_settings ("io.github.paullinux_gn.Vinder.compression");
    nautilus_window_state = new_vinder_settings ("io.github.paullinux_gn.Vinder.window-state");
    nautilus_icon_view_preferences = new_vinder_settings ("io.github.paullinux_gn.Vinder.icon-view");
    nautilus_list_view_preferences = new_vinder_settings ("io.github.paullinux_gn.Vinder.list-view");
    /* Vinder keeps its own settings; copy the old shared ones over, once. */
#if !VINDER_SPLIT_PROFILE
    vinder_settings_migrate_legacy (nautilus_preferences,
                                    nautilus_compression_preferences,
                                    nautilus_icon_view_preferences,
                                    nautilus_list_view_preferences,
                                    nautilus_window_state);
#endif
    /* Some settings such as show hidden files are shared between Nautilus and GTK file chooser */
    gtk_filechooser_preferences = g_settings_new_with_path ("org.gtk.gtk4.Settings.FileChooser",
                                                            "/org/gtk/gtk4/settings/file-chooser/");
    gnome_lockdown_preferences = g_settings_new ("org.gnome.desktop.lockdown");
    gnome_interface_preferences = g_settings_new ("org.gnome.desktop.interface");
    gnome_privacy_preferences = g_settings_new ("org.gnome.desktop.privacy");
    localsearch_preferences = g_settings_new ("org.freedesktop.Tracker3.Miner.Files");

    if (check_schema_available (FILE_SHARING_SCHEMA_ID) &&
        check_schema_available (FILE_SHARING_SERVICE_SCHEMA_ID))
    {
        gnome_user_share_preferences = g_settings_new_with_path (FILE_SHARING_SERVICE_SCHEMA_ID,
                                                                 FILE_SHARING_SERVICE_PATH);
    }

    g_signal_connect_swapped (nautilus_preferences,
                              "changed::" NAUTILUS_PREFERENCES_MOUSE_BACK_BUTTON,
                              G_CALLBACK (mouse_back_button_changed),
                              NULL);

    g_signal_connect_swapped (nautilus_preferences,
                              "changed::" NAUTILUS_PREFERENCES_MOUSE_FORWARD_BUTTON,
                              G_CALLBACK (mouse_forward_button_changed),
                              NULL);

    g_signal_connect_swapped (nautilus_preferences,
                              "changed::" NAUTILUS_PREFERENCES_MOUSE_USE_EXTRA_BUTTONS,
                              G_CALLBACK (use_extra_mouse_buttons_changed),
                              NULL);
}

guint
nautilus_global_preferences_get_back_button (void)
{
    return mouse_back_button;
}

guint
nautilus_global_preferences_get_forward_button (void)
{
    return mouse_forward_button;
}

gboolean
nautilus_global_preferences_get_use_extra_buttons (void)
{
    return mouse_extra_buttons;
}
