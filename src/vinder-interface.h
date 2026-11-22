/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "nautilus-application.h"

void vinder_interface_setup (NautilusApplication *application);
void vinder_interface_apply_window_appearance (GtkWindow *window);

/* The windows report the real size of the toolbar and of the list's column header, so
 * the blur stylesheet can leave exactly the right room for them. */
void vinder_interface_set_top_bar_height (NautilusApplication *application, int height);
void vinder_interface_set_column_header_height (NautilusApplication *application, int height);
