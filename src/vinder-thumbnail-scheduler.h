/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define VINDER_TYPE_THUMBNAIL_SCHEDULER (vinder_thumbnail_scheduler_get_type ())
G_DECLARE_FINAL_TYPE (VinderThumbnailScheduler, vinder_thumbnail_scheduler,
                      VINDER, THUMBNAIL_SCHEDULER, GObject)

/* Called once for every file found in a watched folder. Return TRUE if work was
 * started: @callback must then be called with @callback_data when it is done.
 * Return FALSE if the file needs nothing. */
typedef gboolean (*VinderThumbnailItemFunc) (GFile              *file,
                                             GFileInfo          *info,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            callback_data,
                                             gpointer            user_data);

/* Walks the watched folders, one file at a time, but only while the user has
 * been inactive for @idle_ms and the scheduler is not paused. Work stops as soon
 * as one of those conditions ends and resumes by itself when they hold again.
 * Folders are scanned again (after a quiet period) when files appear in them. */
VinderThumbnailScheduler *vinder_thumbnail_scheduler_new (const char              *attributes,
                                                          guint                    idle_ms,
                                                          VinderThumbnailItemFunc  item_func,
                                                          gpointer                 user_data);

/* Replaces the set of watched folders (absolute paths). */
void vinder_thumbnail_scheduler_set_folders (VinderThumbnailScheduler *self,
                                             const char * const       *paths);

/* Tell the scheduler that the user just did something. */
void vinder_thumbnail_scheduler_notify_activity (VinderThumbnailScheduler *self);

/* While paused no new work starts (power saver, thumbnails turned off...). */
void vinder_thumbnail_scheduler_set_paused (VinderThumbnailScheduler *self,
                                            gboolean                  paused);

/* Cancels running work and stops watching. The object can then be released. */
void vinder_thumbnail_scheduler_stop (VinderThumbnailScheduler *self);

G_END_DECLS
