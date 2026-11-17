/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vinder-thumbnail-preparation.h"
#include <gtk/gtk.h>

#include "nautilus-global-preferences.h"
#include "nautilus-thumbnails.h"

#define IDLE_SECONDS 5

#define THUMBNAIL_ATTRIBUTES G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
    G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN

typedef struct
{
    GApplication *application;
    GFile *folder;
    GFileMonitor *monitor;
    GCancellable *cancellable;
    GFileEnumerator *enumerator;
    GList *batch;
    guint source_id;
    gboolean scanning;
    gboolean rescan;
    gboolean stopped;
} ThumbnailPreparation;

static void schedule_scan (ThumbnailPreparation *self);
static void schedule_next_file (ThumbnailPreparation *self);

static gboolean
on_input_event (GtkEventControllerLegacy *controller,
                GdkEvent                 *event,
                GApplication             *application)
{
    GTimer *idle_timer = g_object_get_data (G_OBJECT (application), "thumbnail-idle-timer");

    g_timer_start (idle_timer);
    return GDK_EVENT_PROPAGATE;
}

static void
watch_window_input (GtkApplication *application,
                    GtkWindow      *window,
                    gpointer        user_data)
{
    GtkEventController *controller = gtk_event_controller_legacy_new ();

    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect_object (controller, "event", G_CALLBACK (on_input_event), application, 0);
    gtk_widget_add_controller (GTK_WIDGET (window), controller);
}

static gboolean
is_idle (ThumbnailPreparation *self)
{
    GTimer *idle_timer = g_object_get_data (G_OBJECT (self->application), "thumbnail-idle-timer");

    return g_timer_elapsed (idle_timer, NULL) >= IDLE_SECONDS;
}

static void
free_preparation (ThumbnailPreparation *self)
{
    g_clear_object (&self->enumerator);
    g_clear_list (&self->batch, g_object_unref);
    g_clear_object (&self->monitor);
    g_object_unref (self->folder);
    g_object_unref (self->cancellable);
    g_free (self);
}

static void
finish_scan (ThumbnailPreparation *self)
{
    g_clear_object (&self->enumerator);
    g_clear_list (&self->batch, g_object_unref);
    self->scanning = FALSE;

    if (self->stopped)
    {
        free_preparation (self);
    }
    else if (self->rescan)
    {
        schedule_scan (self);
    }
}

static void
on_batch_ready (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
    ThumbnailPreparation *self = user_data;

    self->batch = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (source), result, NULL);
    if (self->stopped || self->batch == NULL)
    {
        finish_scan (self);
        return;
    }

    schedule_next_file (self);
}

static void
on_thumbnail_ready (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    ThumbnailPreparation *self = user_data;

    if (self->stopped)
    {
        finish_scan (self);
        return;
    }

    schedule_next_file (self);
}

static gboolean
prepare_next_thumbnail (gpointer user_data)
{
    ThumbnailPreparation *self = user_data;

    self->source_id = 0;
    if (!is_idle (self))
    {
        self->source_id = g_timeout_add_seconds_full (G_PRIORITY_LOW, 1,
                                                      prepare_next_thumbnail, self, NULL);
        return G_SOURCE_REMOVE;
    }

    if (g_settings_get_enum (nautilus_preferences, NAUTILUS_PREFERENCES_SHOW_FILE_THUMBNAILS) ==
        NAUTILUS_SPEED_TRADEOFF_NEVER)
    {
        finish_scan (self);
        return G_SOURCE_REMOVE;
    }

    if (self->batch == NULL)
    {
        g_file_enumerator_next_files_async (self->enumerator, 32, G_PRIORITY_LOW,
                                            self->cancellable, on_batch_ready, self);
        return G_SOURCE_REMOVE;
    }

    g_autoptr (GFileInfo) info = self->batch->data;
    self->batch = g_list_delete_link (self->batch, self->batch);

    const char *content_type = g_file_info_get_content_type (info);
    guint64 size_limit = g_settings_get_uint64 (nautilus_preferences,
                                               NAUTILUS_PREFERENCES_FILE_THUMBNAIL_LIMIT) * 1000000;
    if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&
        !g_file_info_get_is_hidden (info) && content_type != NULL &&
        g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
        !g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID))
    {
        g_autofree char *mime_type = g_content_type_get_mime_type (content_type);
        g_autoptr (GFile) file = g_file_enumerator_get_child (self->enumerator, info);
        g_autofree char *uri = g_file_get_uri (file);
        time_t mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);

        if (mime_type != NULL &&
            ((guint64) g_file_info_get_size (info) <= size_limit ||
             !nautilus_thumbnail_is_mimetype_limited_by_size (mime_type)) &&
            nautilus_can_thumbnail (uri, mime_type, mtime))
        {
            nautilus_create_thumbnail_async (uri, mime_type, mtime,
                                             self->cancellable, on_thumbnail_ready, self);
            return G_SOURCE_REMOVE;
        }
    }

    schedule_next_file (self);
    return G_SOURCE_REMOVE;
}

static void
schedule_next_file (ThumbnailPreparation *self)
{
    self->source_id = g_idle_add_full (G_PRIORITY_LOW, prepare_next_thumbnail, self, NULL);
}

static void
on_folder_opened (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
    ThumbnailPreparation *self = user_data;

    self->enumerator = g_file_enumerate_children_finish (G_FILE (source), result, NULL);
    if (self->stopped || self->enumerator == NULL)
    {
        finish_scan (self);
        return;
    }

    schedule_next_file (self);
}

static void
on_folder_changed (GFileMonitor      *monitor,
                   GFile             *file,
                   GFile             *other_file,
                   GFileMonitorEvent  event,
                   gpointer           user_data)
{
    ThumbnailPreparation *self = user_data;

    switch (event)
    {
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_RENAMED:
            self->rescan = TRUE;
            if (!self->scanning)
            {
                schedule_scan (self);
            }
            break;
        default:
            break;
    }
}

static gboolean
start_scan (gpointer user_data)
{
    ThumbnailPreparation *self = user_data;

    if (!is_idle (self))
    {
        return G_SOURCE_CONTINUE;
    }

    self->source_id = 0;
    self->rescan = FALSE;

    if (self->monitor == NULL)
    {
        self->monitor = g_file_monitor_directory (self->folder, G_FILE_MONITOR_WATCH_MOVES,
                                                  self->cancellable, NULL);
        if (self->monitor != NULL)
        {
            g_signal_connect (self->monitor, "changed", G_CALLBACK (on_folder_changed), self);
        }
    }

    self->scanning = TRUE;
    g_file_enumerate_children_async (self->folder, THUMBNAIL_ATTRIBUTES,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_LOW,
                                     self->cancellable, on_folder_opened, self);
    return G_SOURCE_REMOVE;
}

static void
schedule_scan (ThumbnailPreparation *self)
{
    g_clear_handle_id (&self->source_id, g_source_remove);
    self->source_id = g_timeout_add_seconds_full (G_PRIORITY_LOW, IDLE_SECONDS, start_scan, self, NULL);
}

static void
on_shutdown (GApplication *application,
             gpointer      user_data)
{
    ThumbnailPreparation *self = user_data;
    gboolean callback_pending = self->scanning && self->source_id == 0;

    self->stopped = TRUE;
    g_clear_handle_id (&self->source_id, g_source_remove);
    if (self->monitor != NULL)
    {
        g_file_monitor_cancel (self->monitor);
    }

    if (callback_pending)
    {
        g_cancellable_cancel (self->cancellable);
    }
    else
    {
        free_preparation (self);
    }
}

void
vinder_thumbnail_preparation_start (GApplication *application)
{
    const char *path = g_get_user_special_dir (G_USER_DIRECTORY_PICTURES);

    if (path == NULL || g_str_equal (path, g_get_home_dir ()) || g_str_equal (path, "/"))
    {
        return;
    }

    ThumbnailPreparation *self = g_new0 (ThumbnailPreparation, 1);

    self->application = application;
    self->folder = g_file_new_for_path (path);
    self->cancellable = g_cancellable_new ();
    g_object_set_data_full (G_OBJECT (application), "thumbnail-idle-timer",
                            g_timer_new (), (GDestroyNotify) g_timer_destroy);
    g_signal_connect (application, "window-added", G_CALLBACK (watch_window_input), NULL);
    g_signal_connect (application, "shutdown", G_CALLBACK (on_shutdown), self);
    schedule_scan (self);
}
