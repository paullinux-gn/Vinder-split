/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "vinder-thumbnail-scheduler.h"

#define IDLE_MS 300
#define ATTRIBUTES "standard::name,standard::type"

typedef struct
{
    GPtrArray *seen;        /* paths item_func was called for, in order */
    gboolean slow;          /* complete items after a delay instead of right away */
    char *dir_a;
    char *dir_b;
} Fixture;

typedef struct
{
    GAsyncReadyCallback callback;
    gpointer callback_data;
} Pending;

static gboolean
complete_item (gpointer user_data)
{
    Pending *pending = user_data;
    g_autoptr (GTask) task = g_task_new (NULL, NULL, pending->callback, pending->callback_data);

    g_task_return_boolean (task, TRUE);
    g_free (pending);
    return G_SOURCE_REMOVE;
}

static gboolean
item_func (GFile              *file,
           GFileInfo          *info,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            callback_data,
           gpointer            user_data)
{
    Fixture *fixture = user_data;
    Pending *pending = g_new0 (Pending, 1);

    g_ptr_array_add (fixture->seen, g_file_get_path (file));

    pending->callback = callback;
    pending->callback_data = callback_data;
    g_timeout_add (fixture->slow ? 10 : 0, complete_item, pending);
    return TRUE;
}

static char *
make_dir_with_files (guint n_files)
{
    char *dir = g_dir_make_tmp ("vinder-scheduler-XXXXXX", NULL);

    g_assert_nonnull (dir);
    for (guint i = 0; i < n_files; i++)
    {
        g_autofree char *name = g_strdup_printf ("file-%03u.txt", i);
        g_autofree char *path = g_build_filename (dir, name, NULL);

        g_assert_true (g_file_set_contents (path, "x", 1, NULL));
    }

    return dir;
}

static void
remove_dir (char *dir)
{
    g_autoptr (GDir) handle = g_dir_open (dir, 0, NULL);
    const char *name;

    while (handle != NULL && (name = g_dir_read_name (handle)) != NULL)
    {
        g_autofree char *path = g_build_filename (dir, name, NULL);

        g_remove (path);
    }

    g_rmdir (dir);
    g_free (dir);
}

static void
fixture_set_up (Fixture *fixture,
                guint    n_files_a,
                guint    n_files_b)
{
    fixture->seen = g_ptr_array_new_with_free_func (g_free);
    fixture->dir_a = make_dir_with_files (n_files_a);
    fixture->dir_b = make_dir_with_files (n_files_b);
}

static void
fixture_tear_down (Fixture *fixture)
{
    g_ptr_array_free (fixture->seen, TRUE);
    remove_dir (fixture->dir_a);
    remove_dir (fixture->dir_b);
}

static gboolean
on_timeout (gpointer user_data)
{
    *(gboolean *) user_data = TRUE;
    return G_SOURCE_REMOVE;
}

/* Runs the main loop until @condition is true. Returns FALSE on timeout. */
static gboolean
run_until (gboolean (*condition) (Fixture *, guint),
           Fixture *fixture,
           guint    arg,
           guint    timeout_ms)
{
    gboolean timed_out = FALSE;
    guint id = g_timeout_add (timeout_ms, on_timeout, &timed_out);

    while (!condition (fixture, arg) && !timed_out)
    {
        g_main_context_iteration (NULL, TRUE);
    }

    if (!timed_out)
    {
        g_source_remove (id);
    }

    return condition (fixture, arg);
}

static void
run_for (guint ms)
{
    gboolean done = FALSE;

    g_timeout_add (ms, on_timeout, &done);
    while (!done)
    {
        g_main_context_iteration (NULL, TRUE);
    }
}

static gboolean
seen_at_least (Fixture *fixture,
               guint    n)
{
    return fixture->seen->len >= n;
}

static gboolean
each_seen_once (Fixture *fixture)
{
    g_autoptr (GHashTable) paths = g_hash_table_new (g_str_hash, g_str_equal);

    for (guint i = 0; i < fixture->seen->len; i++)
    {
        if (!g_hash_table_add (paths, g_ptr_array_index (fixture->seen, i)))
        {
            return FALSE;
        }
    }

    return TRUE;
}

static void
test_scans_folder_once_idle (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    gint64 start = g_get_monotonic_time ();

    fixture_set_up (&fixture, 3, 0);
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);

    g_assert_true (run_until (seen_at_least, &fixture, 3, 3000));
    /* It waited for the user to be idle first. */
    g_assert_cmpint (g_get_monotonic_time () - start, >=, (IDLE_MS - 20) * 1000);

    run_for (300);
    g_assert_cmpuint (fixture.seen->len, ==, 3);
    g_assert_true (each_seen_once (&fixture));

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static void
test_paused_then_resumed (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };

    fixture_set_up (&fixture, 3, 0);
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_paused (scheduler, TRUE);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);

    run_for (500);
    g_assert_cmpuint (fixture.seen->len, ==, 0);

    vinder_thumbnail_scheduler_set_paused (scheduler, FALSE);
    g_assert_true (run_until (seen_at_least, &fixture, 3, 3000));

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static gboolean
poke (gpointer user_data)
{
    vinder_thumbnail_scheduler_notify_activity (user_data);
    return G_SOURCE_CONTINUE;
}

static void
test_waits_for_user_to_be_idle (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    guint poke_id;

    fixture_set_up (&fixture, 3, 0);
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    poke_id = g_timeout_add (10, poke, scheduler);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);

    /* The user keeps being active: nothing happens. */
    run_for (600);
    g_assert_cmpuint (fixture.seen->len, ==, 0);

    /* The user stops: work starts by itself. */
    g_source_remove (poke_id);
    g_assert_true (run_until (seen_at_least, &fixture, 3, 3000));

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static void
test_activity_pauses_running_scan (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    guint seen_during_activity;
    guint poke_id;

    fixture_set_up (&fixture, 60, 0);
    fixture.slow = TRUE;
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);

    g_assert_true (run_until (seen_at_least, &fixture, 5, 3000));

    poke_id = g_timeout_add (10, poke, scheduler);
    run_for (100);                      /* let an in-flight item finish */
    seen_during_activity = fixture.seen->len;
    run_for (500);
    g_assert_cmpuint (fixture.seen->len, <=, seen_during_activity + 1);

    g_source_remove (poke_id);
    g_assert_true (run_until (seen_at_least, &fixture, 60, 5000));
    run_for (200);
    g_assert_cmpuint (fixture.seen->len, ==, 60);
    g_assert_true (each_seen_once (&fixture));

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static void
test_removing_the_folder_aborts_the_scan (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    guint seen_at_removal;

    fixture_set_up (&fixture, 100, 0);
    fixture.slow = TRUE;
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);

    g_assert_true (run_until (seen_at_least, &fixture, 5, 3000));
    vinder_thumbnail_scheduler_set_folders (scheduler, NULL);
    seen_at_removal = fixture.seen->len;

    run_for (400);
    g_assert_cmpuint (fixture.seen->len, <=, seen_at_removal + 1);

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static void
test_folder_added_later (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *only_a[] = { NULL, NULL };
    const char *both[] = { NULL, NULL, NULL };

    fixture_set_up (&fixture, 2, 3);
    only_a[0] = fixture.dir_a;
    both[0] = fixture.dir_a;
    both[1] = fixture.dir_b;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);

    vinder_thumbnail_scheduler_set_folders (scheduler, only_a);
    g_assert_true (run_until (seen_at_least, &fixture, 2, 3000));

    vinder_thumbnail_scheduler_set_folders (scheduler, both);
    g_assert_true (run_until (seen_at_least, &fixture, 5, 3000));

    /* The first folder was not walked again. */
    run_for (300);
    g_assert_cmpuint (fixture.seen->len, ==, 5);
    g_assert_true (each_seen_once (&fixture));

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static void
test_new_file_is_picked_up (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    g_autoptr (GFile) folder = NULL;
    g_autoptr (GFileMonitor) probe = NULL;
    g_autofree char *new_file = NULL;

    fixture_set_up (&fixture, 2, 0);
    folder = g_file_new_for_path (fixture.dir_a);
    probe = g_file_monitor_directory (folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (probe == NULL)
    {
        fixture_tear_down (&fixture);
        g_test_skip ("Cannot monitor directories here");
        return;
    }

    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);
    g_assert_true (run_until (seen_at_least, &fixture, 2, 3000));

    new_file = g_build_filename (fixture.dir_a, "new.txt", NULL);
    g_assert_true (g_file_set_contents (new_file, "x", 1, NULL));

    g_assert_true (run_until (seen_at_least, &fixture, 3, 5000));
    g_assert_cmpstr (g_ptr_array_index (fixture.seen, 2), ==, new_file);

    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);
    fixture_tear_down (&fixture);
}

static gboolean
weak_cleared (Fixture *fixture,
              guint    arg)
{
    return *(gpointer *) fixture->dir_b == NULL;   /* dir_b slot reused as the weak pointer */
}

static void
check_scheduler_is_freed (Fixture                  *fixture,
                          VinderThumbnailScheduler *scheduler)
{
    gpointer weak = scheduler;
    char *saved = fixture->dir_b;

    g_object_add_weak_pointer (G_OBJECT (scheduler), &weak);
    vinder_thumbnail_scheduler_stop (scheduler);
    g_object_unref (scheduler);

    fixture->dir_b = (char *) &weak;
    g_assert_true (run_until (weak_cleared, fixture, 0, 3000));
    fixture->dir_b = saved;
}

static void
test_stop_releases_a_running_scheduler (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    guint seen_at_stop;

    fixture_set_up (&fixture, 100, 0);
    fixture.slow = TRUE;
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);
    g_assert_true (run_until (seen_at_least, &fixture, 5, 3000));

    seen_at_stop = fixture.seen->len;
    check_scheduler_is_freed (&fixture, scheduler);

    run_for (200);
    g_assert_cmpuint (fixture.seen->len, <=, seen_at_stop + 1);
    fixture_tear_down (&fixture);
}

static void
test_stop_releases_a_suspended_scheduler (void)
{
    Fixture fixture = { 0 };
    VinderThumbnailScheduler *scheduler;
    const char *folders[] = { NULL, NULL };
    guint seen_when_paused;

    fixture_set_up (&fixture, 100, 0);
    fixture.slow = TRUE;
    folders[0] = fixture.dir_a;
    scheduler = vinder_thumbnail_scheduler_new (ATTRIBUTES, IDLE_MS, item_func, &fixture);
    vinder_thumbnail_scheduler_set_folders (scheduler, folders);
    g_assert_true (run_until (seen_at_least, &fixture, 5, 3000));

    /* Pause in the middle of a folder: the pass is suspended, not finished. */
    vinder_thumbnail_scheduler_set_paused (scheduler, TRUE);
    run_for (200);
    seen_when_paused = fixture.seen->len;
    run_for (200);
    g_assert_cmpuint (fixture.seen->len, ==, seen_when_paused);
    g_assert_cmpuint (seen_when_paused, <, 100);

    check_scheduler_is_freed (&fixture, scheduler);
    fixture_tear_down (&fixture);
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/vinder-thumbnail-scheduler/scans-folder-once-idle",
                     test_scans_folder_once_idle);
    g_test_add_func ("/vinder-thumbnail-scheduler/paused-then-resumed",
                     test_paused_then_resumed);
    g_test_add_func ("/vinder-thumbnail-scheduler/waits-for-user-to-be-idle",
                     test_waits_for_user_to_be_idle);
    g_test_add_func ("/vinder-thumbnail-scheduler/activity-pauses-running-scan",
                     test_activity_pauses_running_scan);
    g_test_add_func ("/vinder-thumbnail-scheduler/removing-folder-aborts-scan",
                     test_removing_the_folder_aborts_the_scan);
    g_test_add_func ("/vinder-thumbnail-scheduler/folder-added-later",
                     test_folder_added_later);
    g_test_add_func ("/vinder-thumbnail-scheduler/new-file-is-picked-up",
                     test_new_file_is_picked_up);
    g_test_add_func ("/vinder-thumbnail-scheduler/stop-releases-running-scheduler",
                     test_stop_releases_a_running_scheduler);
    g_test_add_func ("/vinder-thumbnail-scheduler/stop-releases-suspended-scheduler",
                     test_stop_releases_a_suspended_scheduler);

    return g_test_run ();
}
