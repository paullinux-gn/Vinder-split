/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "vinder-thumbnail-scheduler.h"

#define BATCH_SIZE 32

typedef struct
{
    char *path;
    GFile *folder;
    GFileMonitor *monitor;
    gboolean dirty;
} Watch;

/* State of one pass over one folder. It lives in the data of a GTask, which keeps
 * the scheduler alive until the pass is over, so callbacks never see freed memory. */
typedef struct
{
    char *path;
    GFile *folder;
    GCancellable *cancellable;
    GFileEnumerator *enumerator;
    GList *batch;
} ScanJob;

struct _VinderThumbnailScheduler
{
    GObject parent_instance;

    char *attributes;
    gint64 idle_us;
    VinderThumbnailItemFunc item_func;
    gpointer user_data;

    GPtrArray *watches;

    /* Folder pass in progress. Holding the task here makes a reference cycle with
     * the scheduler; it ends when the pass finishes or is aborted. */
    GTask *active;
    gboolean waiting;           /* @active is suspended until conditions allow */

    gint64 last_activity_us;
    gint64 not_before_us;       /* quiet period after folder changes */
    gboolean paused;
    gboolean stopped;
    guint wait_source;
};

G_DEFINE_FINAL_TYPE (VinderThumbnailScheduler, vinder_thumbnail_scheduler, G_TYPE_OBJECT)

static void arm (VinderThumbnailScheduler *self);
static void schedule_step (GTask *task);

static void
watch_free (Watch *watch)
{
    if (watch->monitor != NULL)
    {
        g_file_monitor_cancel (watch->monitor);
        g_object_unref (watch->monitor);
    }
    g_object_unref (watch->folder);
    g_free (watch->path);
    g_free (watch);
}

static void
scan_job_free (ScanJob *job)
{
    g_clear_object (&job->enumerator);
    g_clear_list (&job->batch, g_object_unref);
    g_object_unref (job->cancellable);
    g_object_unref (job->folder);
    g_free (job->path);
    g_free (job);
}

static gint64
ready_time (VinderThumbnailScheduler *self)
{
    return MAX (self->last_activity_us + self->idle_us, self->not_before_us);
}

static gboolean
is_ready (VinderThumbnailScheduler *self)
{
    return !self->stopped && !self->paused &&
           g_get_monotonic_time () >= ready_time (self);
}

static Watch *
find_watch (VinderThumbnailScheduler *self,
            const char               *path)
{
    for (guint i = 0; i < self->watches->len; i++)
    {
        Watch *watch = g_ptr_array_index (self->watches, i);

        if (g_str_equal (watch->path, path))
        {
            return watch;
        }
    }

    return NULL;
}

static Watch *
next_dirty_watch (VinderThumbnailScheduler *self)
{
    for (guint i = 0; i < self->watches->len; i++)
    {
        Watch *watch = g_ptr_array_index (self->watches, i);

        if (watch->dirty)
        {
            return watch;
        }
    }

    return NULL;
}

/* ---- one pass over one folder ---- */

static void
on_item_done (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
    GTask *task = user_data;

    schedule_step (task);
    g_object_unref (task);
}

static void
on_batch_ready (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
    GTask *task = user_data;
    ScanJob *job = g_task_get_task_data (task);
    g_autoptr (GError) error = NULL;

    job->batch = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (source), result, &error);
    if (error != NULL)
    {
        g_task_return_error (task, g_steal_pointer (&error));
    }
    else if (job->batch == NULL)
    {
        g_task_return_boolean (task, TRUE);    /* end of the folder */
    }
    else
    {
        schedule_step (task);
    }

    g_object_unref (task);
}

static gboolean
scan_step (gpointer user_data)
{
    /* The idle source handed us one reference; it is released when we return. */
    g_autoptr (GTask) task = user_data;
    VinderThumbnailScheduler *self = g_task_get_source_object (task);
    ScanJob *job = g_task_get_task_data (task);
    g_autoptr (GFileInfo) info = NULL;
    g_autoptr (GFile) file = NULL;
    GTask *item_reference;

    if (g_task_return_error_if_cancelled (task))
    {
        return G_SOURCE_REMOVE;
    }

    if (!is_ready (self))
    {
        /* The user came back, or we were paused: stay where we are. */
        self->waiting = TRUE;
        arm (self);
        return G_SOURCE_REMOVE;
    }

    if (job->batch == NULL)
    {
        g_file_enumerator_next_files_async (job->enumerator, BATCH_SIZE, G_PRIORITY_LOW,
                                            job->cancellable, on_batch_ready, g_object_ref (task));
        return G_SOURCE_REMOVE;
    }

    info = job->batch->data;
    job->batch = g_list_delete_link (job->batch, job->batch);
    file = g_file_enumerator_get_child (job->enumerator, info);

    item_reference = g_object_ref (task);
    if (!self->item_func (file, info, job->cancellable, on_item_done,
                          item_reference, self->user_data))
    {
        /* Nothing to do for this file, so no callback will release the reference. */
        g_object_unref (item_reference);
        schedule_step (task);
    }

    return G_SOURCE_REMOVE;
}

static void
schedule_step (GTask *task)
{
    g_idle_add_full (G_PRIORITY_LOW, scan_step, g_object_ref (task), NULL);
}

static void
on_folder_opened (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
    GTask *task = user_data;
    ScanJob *job = g_task_get_task_data (task);
    g_autoptr (GError) error = NULL;

    job->enumerator = g_file_enumerate_children_finish (G_FILE (source), result, &error);
    if (error != NULL)
    {
        g_task_return_error (task, g_steal_pointer (&error));
    }
    else
    {
        schedule_step (task);
    }

    g_object_unref (task);
}

static void
on_scan_done (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
    VinderThumbnailScheduler *self = VINDER_THUMBNAIL_SCHEDULER (source);

    /* The outcome does not matter: a failed folder is simply tried again when it changes. */
    g_task_propagate_boolean (G_TASK (result), NULL);

    if (self->active == G_TASK (result))
    {
        g_clear_object (&self->active);
        self->waiting = FALSE;
    }

    arm (self);
}

static void
start_scan (VinderThumbnailScheduler *self,
            Watch                    *watch)
{
    ScanJob *job = g_new0 (ScanJob, 1);
    g_autoptr (GTask) task = NULL;

    job->path = g_strdup (watch->path);
    job->folder = g_object_ref (watch->folder);
    job->cancellable = g_cancellable_new ();

    /* Changes made from now on mark the folder dirty again. */
    watch->dirty = FALSE;

    task = g_task_new (self, job->cancellable, on_scan_done, NULL);
    g_task_set_task_data (task, job, (GDestroyNotify) scan_job_free);
    self->active = g_object_ref (task);
    self->waiting = FALSE;

    g_file_enumerate_children_async (job->folder, self->attributes,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_LOW,
                                     job->cancellable, on_folder_opened, g_object_ref (task));
}

static void
abort_scan (VinderThumbnailScheduler *self)
{
    g_autoptr (GTask) task = g_steal_pointer (&self->active);

    if (task == NULL)
    {
        return;
    }

    g_cancellable_cancel (((ScanJob *) g_task_get_task_data (task))->cancellable);

    if (self->waiting)
    {
        /* Nothing is in flight that would notice the cancellation, so finish it here. */
        self->waiting = FALSE;
        g_task_return_error_if_cancelled (task);
    }
}

/* ---- deciding when to work ---- */

static void
run (VinderThumbnailScheduler *self)
{
    if (self->active != NULL)
    {
        if (self->waiting)
        {
            self->waiting = FALSE;
            schedule_step (self->active);
        }
    }
    else
    {
        Watch *watch = next_dirty_watch (self);

        if (watch != NULL)
        {
            start_scan (self, watch);
        }
    }
}

static gboolean
on_wait_elapsed (gpointer user_data)
{
    VinderThumbnailScheduler *self = user_data;

    self->wait_source = 0;

    /* Activity may have moved the goal posts while we slept: arm again for what is left. */
    if (is_ready (self))
    {
        run (self);
    }
    else
    {
        arm (self);
    }

    return G_SOURCE_REMOVE;
}

/* Makes sure exactly one timer is pending when there is work that is only waiting for
 * the user to be idle, and none otherwise. */
static void
arm (VinderThumbnailScheduler *self)
{
    gint64 delay_us;

    g_clear_handle_id (&self->wait_source, g_source_remove);

    if (self->stopped || self->paused)
    {
        return;
    }

    if (self->active != NULL ? !self->waiting : next_dirty_watch (self) == NULL)
    {
        return;     /* running by itself, or nothing to do */
    }

    delay_us = ready_time (self) - g_get_monotonic_time ();
    self->wait_source = g_timeout_add_full (G_PRIORITY_LOW,
                                            delay_us > 0 ? (guint) ((delay_us + 999) / 1000) : 0,
                                            on_wait_elapsed, self, NULL);
}

/* ---- folders ---- */

static void
on_folder_changed (GFileMonitor      *monitor,
                   GFile             *file,
                   GFile             *other_file,
                   GFileMonitorEvent  event,
                   gpointer           user_data)
{
    VinderThumbnailScheduler *self = user_data;

    switch (event)
    {
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_RENAMED:
        {
            for (guint i = 0; i < self->watches->len; i++)
            {
                Watch *watch = g_ptr_array_index (self->watches, i);

                if (watch->monitor == monitor)
                {
                    watch->dirty = TRUE;
                }
            }

            /* Wait for things to settle before looking again. */
            self->not_before_us = g_get_monotonic_time () + self->idle_us;
            arm (self);
            break;
        }

        default:
            break;
    }
}

static Watch *
watch_new (VinderThumbnailScheduler *self,
           const char               *path)
{
    Watch *watch = g_new0 (Watch, 1);

    watch->path = g_strdup (path);
    watch->folder = g_file_new_for_path (path);
    watch->dirty = TRUE;
    watch->monitor = g_file_monitor_directory (watch->folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (watch->monitor != NULL)
    {
        g_signal_connect (watch->monitor, "changed", G_CALLBACK (on_folder_changed), self);
    }

    return watch;
}

void
vinder_thumbnail_scheduler_set_folders (VinderThumbnailScheduler *self,
                                        const char * const       *paths)
{
    g_return_if_fail (VINDER_IS_THUMBNAIL_SCHEDULER (self));

    if (self->stopped)
    {
        return;
    }

    /* Drop folders that are no longer wanted. */
    for (guint i = self->watches->len; i > 0; i--)
    {
        Watch *watch = g_ptr_array_index (self->watches, i - 1);

        if (paths == NULL || !g_strv_contains (paths, watch->path))
        {
            if (self->active != NULL &&
                g_str_equal (((ScanJob *) g_task_get_task_data (self->active))->path, watch->path))
            {
                abort_scan (self);
            }

            g_ptr_array_remove_index (self->watches, i - 1);
        }
    }

    /* Add the new ones; they get a first look after the quiet period. */
    for (guint i = 0; paths != NULL && paths[i] != NULL; i++)
    {
        if (find_watch (self, paths[i]) == NULL)
        {
            g_ptr_array_add (self->watches, watch_new (self, paths[i]));
            self->not_before_us = g_get_monotonic_time () + self->idle_us;
        }
    }

    arm (self);
}

/* ---- public API ---- */

void
vinder_thumbnail_scheduler_notify_activity (VinderThumbnailScheduler *self)
{
    g_return_if_fail (VINDER_IS_THUMBNAIL_SCHEDULER (self));

    self->last_activity_us = g_get_monotonic_time ();
}

void
vinder_thumbnail_scheduler_set_paused (VinderThumbnailScheduler *self,
                                       gboolean                  paused)
{
    g_return_if_fail (VINDER_IS_THUMBNAIL_SCHEDULER (self));

    if (self->paused != !!paused)
    {
        self->paused = !!paused;
        arm (self);
    }
}

void
vinder_thumbnail_scheduler_stop (VinderThumbnailScheduler *self)
{
    g_return_if_fail (VINDER_IS_THUMBNAIL_SCHEDULER (self));

    self->stopped = TRUE;
    g_clear_handle_id (&self->wait_source, g_source_remove);
    abort_scan (self);
    g_ptr_array_set_size (self->watches, 0);
}

VinderThumbnailScheduler *
vinder_thumbnail_scheduler_new (const char              *attributes,
                                guint                    idle_ms,
                                VinderThumbnailItemFunc  item_func,
                                gpointer                 user_data)
{
    VinderThumbnailScheduler *self = g_object_new (VINDER_TYPE_THUMBNAIL_SCHEDULER, NULL);

    self->attributes = g_strdup (attributes);
    self->idle_us = (gint64) idle_ms * 1000;
    self->item_func = item_func;
    self->user_data = user_data;
    self->last_activity_us = g_get_monotonic_time ();
    self->not_before_us = self->last_activity_us;

    return self;
}

static void
vinder_thumbnail_scheduler_dispose (GObject *object)
{
    VinderThumbnailScheduler *self = VINDER_THUMBNAIL_SCHEDULER (object);

    vinder_thumbnail_scheduler_stop (self);

    G_OBJECT_CLASS (vinder_thumbnail_scheduler_parent_class)->dispose (object);
}

static void
vinder_thumbnail_scheduler_finalize (GObject *object)
{
    VinderThumbnailScheduler *self = VINDER_THUMBNAIL_SCHEDULER (object);

    g_ptr_array_free (self->watches, TRUE);
    g_free (self->attributes);

    G_OBJECT_CLASS (vinder_thumbnail_scheduler_parent_class)->finalize (object);
}

static void
vinder_thumbnail_scheduler_class_init (VinderThumbnailSchedulerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = vinder_thumbnail_scheduler_dispose;
    object_class->finalize = vinder_thumbnail_scheduler_finalize;
}

static void
vinder_thumbnail_scheduler_init (VinderThumbnailScheduler *self)
{
    self->watches = g_ptr_array_new_with_free_func ((GDestroyNotify) watch_free);
}
