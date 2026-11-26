/*
 * SPDX-FileCopyrightText: 1999, 2000, 2004 Red Hat, Inc.
 * SPDX-FileCopyrightText: 1999, 2000, 2001 Eazel, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Authors: Elliot Lee <sopwith@redhat.com>
 *          John Sullivan <sullivan@eazel.com>
 *          Alexander Larsson <alexl@redhat.com>
 */

/* nautilus-window.c: Implementation of the main window object */
#define G_LOG_DOMAIN "nautilus-window"

#include <config.h>

#include "nautilus-window.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include "nautilus-application.h"
#include "nautilus-dnd.h"
#include "nautilus-enums.h"
#include "nautilus-file.h"
#include "nautilus-file-operations.h"
#include "nautilus-file-undo-manager.h"
#include "nautilus-file-undo-operations.h"
#include "nautilus-files-view.h"
#include "nautilus-global-preferences.h"
#include "nautilus-network-address-bar.h"
#include "nautilus-module.h"
#include "nautilus-progress-indicator.h"
#include "nautilus-scheme.h"
#include "nautilus-shortcut-manager.h"
#include "nautilus-sidebar.h"
#include "nautilus-toolbar.h"
#include "nautilus-ui-utilities.h"
#include "nautilus-view-info.h"
#include "nautilus-window-slot.h"
#include "vinder-interface.h"

static void nautilus_window_initialize_actions (NautilusWindow *window);
static void nautilus_window_sync_location_widgets (NautilusWindow *window);
static void update_cursor (NautilusWindow *window);
static void
set_active_slot (NautilusWindow     *window,
                 NautilusWindowSlot *new_slot);
static void window_slot_close (NautilusWindow     *window,
                               NautilusWindowSlot *slot);
static void remove_slot_from_window (NautilusWindowSlot *slot,
                                     NautilusWindow     *window);
static void action_toggle_split_view (GSimpleAction *action,
                                      GVariant      *value,
                                      gpointer       user_data);
static void toggle_split_view_internal (GSimpleAction *action,
                                        gboolean       enabled,
                                        gboolean       create_seed_slot,
                                        NautilusWindow *window);
static void action_activate_split_view (GSimpleAction *action,
                                        GVariant      *state,
                                        gpointer       user_data);
static void action_focus_other_pane (GSimpleAction *action,
                                     GVariant      *state,
                                     gpointer       user_data);
static void action_move_tab_to_other_pane (GSimpleAction *action,
                                           GVariant      *state,
                                           gpointer       user_data);
static void update_tab_strip (NautilusWindow *window,
                              AdwTabView     *tab_view,
                              GtkBox         *tab_strip);
static void update_tab_strip_selection (NautilusWindow *window,
                                        AdwTabView     *tab_view,
                                        GtkBox         *tab_strip);
static gboolean is_layout_reversed (void);

/* Sanity check: highest mouse button value I could find was 14. 5 is our
 * lower threshold (well-documented to be the one of the button events for the
 * scrollwheel), so it's hardcoded in the functions below. However, if you have
 * a button that registers higher and want to map it, file a bug and
 * we'll move the bar. Makes you wonder why the X guys don't have
 * defined values for these like the XKB stuff, huh?
 */

struct _NautilusWindow
{
    AdwApplicationWindow parent_instance;

    GMenuModel *undo_redo_section;

    AdwTabOverview *tab_overview;
    AdwTabView *tab_view;
    AdwTabView *secondary_tab_view;
    GtkWidget *main_tab_bar;
    GtkWidget *secondary_tab_bar;
    GtkWidget *secondary_tab_bar_container;
    GtkBox *main_tab_strip;
    GtkBox *secondary_tab_strip;
    GtkPaned *tab_bars_paned;
    AdwTabButton *tab_button;
    GtkWidget *secondary_pane;
    AdwTabPage *menu_page;

    GList *slots;
    NautilusWindowSlot *active_slot; /* weak reference */

    GtkWidget *split_view;
    AdwToolbarView *files_toolbar_view;
    GtkPaned *paned;
    GtkWidget *main_pane;
    gboolean split_enabled;
    gboolean secondary_is_active;
    gboolean transferring_pages;
    gboolean closing_page;
    gboolean closing;
    guint collapse_source_id;

    /* Side Pane */
    NautilusSidebar *places_sidebar;

    /* Notifications */
    AdwToastOverlay *toast_overlay;
    AdwToast *last_undo_toast;

    /* Toolbar */
    GtkWidget *toolbar;
    AdwWindowTitle *window_title;

    GtkWidget *network_address_bar;

    GQueue *tab_data_queue;

    /* Pad controller which holds a reference to the window. Kept around to
     * break reference-counting cycles during finalization. */
    GtkPadController *pad_controller;
};

static AdwTabView *
get_active_tab_view (NautilusWindow *window)
{
    AdwTabView *active = window->split_enabled && window->secondary_is_active
                         ? window->secondary_tab_view : window->tab_view;
    AdwTabView *other = active == window->tab_view
                        ? window->secondary_tab_view : window->tab_view;

    if (window->split_enabled && active != NULL && other != NULL &&
        adw_tab_view_get_n_pages (active) == 0 && adw_tab_view_get_n_pages (other) > 0)
    {
        return other;
    }

    return active;
}

static AdwTabView *
get_tab_view_for_slot (NautilusWindow     *window,
                       NautilusWindowSlot *slot)
{
    GtkWidget *tab_view = gtk_widget_get_ancestor (GTK_WIDGET (slot), ADW_TYPE_TAB_VIEW);

    if (tab_view == GTK_WIDGET (window->tab_view))
    {
        return window->tab_view;
    }

    if (window->secondary_tab_view != NULL &&
        tab_view == GTK_WIDGET (window->secondary_tab_view))
    {
        return window->secondary_tab_view;
    }

    return NULL;
}

static void
update_shared_tab_widgets (NautilusWindow *window)
{
    AdwTabView *view = get_active_tab_view (window);

    adw_tab_button_set_view (window->tab_button, view);
    adw_tab_overview_set_view (window->tab_overview, view);
}

static void
set_active_pane (NautilusWindow *window,
                 gboolean        secondary)
{
    AdwTabView *view;
    AdwTabPage *page;

    if (secondary && !window->split_enabled)
    {
        return;
    }

    window->secondary_is_active = secondary;
    if (secondary)
    {
        gtk_widget_remove_css_class (window->main_pane, "active-pane");
        gtk_widget_add_css_class (window->secondary_pane, "active-pane");
    }
    else
    {
        gtk_widget_remove_css_class (window->secondary_pane, "active-pane");
        gtk_widget_add_css_class (window->main_pane, "active-pane");
    }
    update_shared_tab_widgets (window);
    view = get_active_tab_view (window);
    page = adw_tab_view_get_selected_page (view);
    if (page != NULL)
    {
        set_active_slot (window, NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page)));
    }
}

enum
{
    LOCATIONS_CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_FINAL_TYPE (NautilusWindow, nautilus_window, ADW_TYPE_APPLICATION_WINDOW);

enum
{
    PROP_0,
    PROP_ACTIVE_SLOT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static const GtkPadActionEntry pad_actions[] =
{
    { GTK_PAD_ACTION_BUTTON, 0, -1, N_("Home"), "go-home" },
    { GTK_PAD_ACTION_BUTTON, 1, -1, N_("New tab"), "new-tab" },
    { GTK_PAD_ACTION_BUTTON, 2, -1, N_("Close current view"), "close-current-view" },
    /* Button number sequence continues in window-slot.c */
};

#ifdef HAVE_MALLOC_TRIM
static guint malloc_trim_idle_id = 0;

static void
malloc_trim_idle_cb (gpointer user_data)
{
    malloc_trim (8 * 1024 * 1024);
    malloc_trim_idle_id = 0;
}

static void
schedule_trim (void)
{
    if (malloc_trim_idle_id == 0)
    {
        malloc_trim_idle_id = g_idle_add_once (malloc_trim_idle_cb, NULL);
    }
}

#endif

static AdwTabPage *
get_current_page (NautilusWindow *window)
{
    if (window->menu_page != NULL)
    {
        return window->menu_page;
    }

    return adw_tab_view_get_selected_page (get_active_tab_view (window));
}

static void
action_open_tab_overview (GSimpleAction *action,
                          GVariant      *state,
                          gpointer       user_data)
{
    NautilusWindow *window = user_data;

    adw_tab_overview_set_open (window->tab_overview, TRUE);
}

static void
action_close_current_view (GSimpleAction *action,
                           GVariant      *state,
                           gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    if (page == NULL)
    {
        return;
    }

    window_slot_close (window, NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page)));
}

static void
action_close_other_tabs (GSimpleAction *action,
                         GVariant      *parameters,
                         gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    if (page == NULL)
    {
        return;
    }

    AdwTabView *view = get_tab_view_for_slot (window,
                                               NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page)));

    adw_tab_view_close_other_pages (view, page);
}

static void
action_go_home (GSimpleAction *action,
                GVariant      *state,
                gpointer       user_data)
{
    NautilusWindow *window;
    GFile *home;

    window = NAUTILUS_WINDOW (user_data);
    home = g_file_new_for_path (g_get_home_dir ());

    nautilus_window_open_location_full (window, home, 0, NULL);

    g_object_unref (home);
}

static void
action_new_tab (GSimpleAction *action,
                GVariant      *state,
                gpointer       user_data)
{
    nautilus_window_new_tab (NAUTILUS_WINDOW (user_data));
}

static void
action_tab_move_left (GSimpleAction *action,
                      GVariant      *state,
                      gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    adw_tab_view_reorder_backward (get_tab_view_for_slot (window,
                                                          NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page))), page);
}

static void
action_tab_move_right (GSimpleAction *action,
                       GVariant      *state,
                       gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    adw_tab_view_reorder_forward (get_tab_view_for_slot (window,
                                                         NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page))), page);
}

static void
action_go_to_tab (GSimpleAction *action,
                  GVariant      *value,
                  gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    gint32 num = g_variant_get_int32 (value);

    AdwTabView *view = get_active_tab_view (window);
    if (num < adw_tab_view_get_n_pages (view))
    {
        AdwTabPage *page = adw_tab_view_get_nth_page (view, num);

        adw_tab_view_set_selected_page (view, page);
    }
}

static void
action_redo (GSimpleAction *action,
             GVariant      *state,
             gpointer       user_data)
{
    NautilusWindow *window = user_data;

    nautilus_file_undo_manager_redo (GTK_WINDOW (window), NULL);
}

static void
action_undo (GSimpleAction *action,
             GVariant      *state,
             gpointer       user_data)
{
    NautilusWindow *window = user_data;

    nautilus_file_undo_manager_undo (GTK_WINDOW (window), NULL);
}

static void
action_show_current_location_menu (GSimpleAction *action,
                                   GVariant      *state,
                                   gpointer       user_data)
{
    NautilusWindow *self = NAUTILUS_WINDOW (user_data);

    nautilus_toolbar_show_current_location_menu (NAUTILUS_TOOLBAR (self->toolbar));
}

static void
on_location_changed (NautilusWindow *window)
{
    nautilus_window_sync_location_widgets (window);
}

static void
on_slot_location_changed (NautilusWindowSlot *slot,
                          GParamSpec         *pspec,
                          NautilusWindow     *window)
{
    if (window->active_slot == slot)
    {
        on_location_changed (window);
    }

    g_signal_emit (window, signals[LOCATIONS_CHANGED], 0);
}

static AdwTabPage *
create_tab_cb (NautilusWindow *window)
{
    nautilus_window_new_tab (window);

    return adw_tab_view_get_selected_page (get_active_tab_view (window));
}

static void
tab_view_setup_menu_cb (AdwTabView     *tab_view,
                        AdwTabPage     *page,
                        NautilusWindow *window)
{
    GAction *move_tab_left_action;
    GAction *move_tab_right_action;
    GAction *restore_tab_action;
    GAction *close_other_tabs_action;
    GAction *move_tab_to_new_window_action;
    GAction *move_tab_to_other_pane_action;
    int position, n_pages;
    gboolean menu_is_closed = (page == NULL);

    if (tab_view == window->secondary_tab_view && window->split_enabled)
    {
        set_active_pane (window, TRUE);
    }
    else if (tab_view == window->tab_view)
    {
        set_active_pane (window, FALSE);
    }

    window->menu_page = page;

    if (!menu_is_closed)
    {
        position = adw_tab_view_get_page_position (tab_view, page);
        n_pages = adw_tab_view_get_n_pages (tab_view);
    }

    move_tab_left_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                       "tab-move-left");
    move_tab_right_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                        "tab-move-right");
    restore_tab_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                     "restore-tab");
    close_other_tabs_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                          "close-other-tabs");
    move_tab_to_new_window_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                                "tab-move-new-window");
    move_tab_to_other_pane_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                                "tab-move-other-pane");

    g_simple_action_set_enabled (G_SIMPLE_ACTION (move_tab_left_action),
                                 menu_is_closed || position > 0);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (move_tab_right_action),
                                 menu_is_closed || position < n_pages - 1);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (restore_tab_action),
                                 g_queue_get_length (window->tab_data_queue) > 0);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (close_other_tabs_action),
                                 menu_is_closed || n_pages > 1);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (move_tab_to_new_window_action),
                                 menu_is_closed || n_pages > 1);
    gboolean can_move_to_other_pane = menu_is_closed;
    if (!menu_is_closed)
    {
        NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
        AdwTabView *source = get_tab_view_for_slot (window, slot);
        can_move_to_other_pane = !window->split_enabled ||
                                 source != window->tab_view || n_pages > 1;
    }
    g_simple_action_set_enabled (G_SIMPLE_ACTION (move_tab_to_other_pane_action),
                                 can_move_to_other_pane);
}

typedef struct
{
    NautilusWindow *window;
    AdwTabView *tab_view;
    AdwTabPage *page;
    GtkBox *tab_strip;
    GtkWidget *tab_bar;
    GtkWidget *chip;
    double drag_start_x;
    double drag_start_y;
} TabItemData;

static void
tab_item_data_free (gpointer user_data)
{
    TabItemData *data = user_data;

    g_clear_object (&data->tab_view);
    g_clear_object (&data->page);
    g_free (data);
}

static TabItemData *
tab_item_data_new (NautilusWindow *window,
                   AdwTabView     *tab_view,
                   GtkBox         *tab_strip,
                   GtkWidget      *tab_bar,
                   AdwTabPage     *page)
{
    TabItemData *data = g_new0 (TabItemData, 1);

    data->window = window;
    data->tab_view = g_object_ref (tab_view);
    data->page = g_object_ref (page);
    data->tab_strip = tab_strip;
    data->tab_bar = tab_bar;

    return data;
}

static void
tab_item_select_cb (GtkToggleButton *button,
                    TabItemData     *data)
{
    if (!gtk_toggle_button_get_active (button) ||
        g_object_get_data (G_OBJECT (button), "syncing-selection") != NULL)
    {
        return;
    }

    if (data->window->split_enabled)
    {
        set_active_pane (data->window,
                         data->tab_view == data->window->secondary_tab_view);
    }

    adw_tab_view_set_selected_page (data->tab_view, data->page);
}

static void
tab_item_close_cb (GtkButton   *button,
                   TabItemData *data)
{
    adw_tab_view_close_page (data->tab_view, data->page);
}

static void
tab_item_menu_closed_cb (GtkPopover  *popover,
                         TabItemData *data)
{
    if (data->window->menu_page == data->page)
    {
        tab_view_setup_menu_cb (data->tab_view, NULL, data->window);
    }
}

static void
tab_item_context_pressed_cb (GtkGestureClick *gesture,
                             int             n_press,
                             double          x,
                             double          y,
                             TabItemData    *data)
{
    GtkWidget *popover = g_object_get_data (G_OBJECT (data->chip), "tab-context-popover");
    GdkRectangle rect = { (int) x, (int) y, 1, 1 };

    if (n_press != 1)
    {
        return;
    }

    tab_view_setup_menu_cb (data->tab_view, data->page, data->window);
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    gtk_popover_popup (GTK_POPOVER (popover));
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
tab_item_drag_begin_cb (GtkGestureDrag *gesture,
                        double          start_x,
                        double          start_y,
                        TabItemData    *data)
{
    data->drag_start_x = start_x;
    data->drag_start_y = start_y;
}

static void
tab_item_drag_end_cb (GtkGestureDrag *gesture,
                      double          offset_x,
                      double          offset_y,
                      TabItemData    *data)
{
    GtkWidget *source = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    graphene_point_t source_point = GRAPHENE_POINT_INIT (data->drag_start_x + offset_x,
                                                          data->drag_start_y + offset_y);
    graphene_point_t bar_point;
    graphene_point_t strip_point;
    GtkAllocation allocation;
    int position = 0;
    int n_pages = adw_tab_view_get_n_pages (data->tab_view);
    AdwTabPage *page;

    /* Tabs may be reordered inside their own pane, but a drag cannot transfer
     * a tab to another pane or detach it into a new window. */
    if (!gtk_widget_compute_point (source, data->tab_bar, &source_point, &bar_point) ||
        bar_point.x < 0 || bar_point.y < 0 ||
        bar_point.x >= gtk_widget_get_width (data->tab_bar) ||
        bar_point.y >= gtk_widget_get_height (data->tab_bar) ||
        !gtk_widget_compute_point (source, GTK_WIDGET (data->tab_strip),
                                   &source_point, &strip_point))
    {
        return;
    }

    for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (data->tab_strip));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        gtk_widget_get_allocation (child, &allocation);
        page = g_object_get_data (G_OBJECT (child), "tab-page");
        if (page == NULL)
        {
            continue;
        }

        if (strip_point.x < allocation.x + allocation.width / 2.0)
        {
            break;
        }

        position++;
    }

    position = CLAMP (position, 0, n_pages - 1);
    if (adw_tab_view_get_page_position (data->tab_view, data->page) != position)
    {
        adw_tab_view_reorder_page (data->tab_view, data->page, position);
    }
}

static void
tab_page_title_changed_cb (AdwTabPage  *page,
                           GParamSpec  *pspec,
                           GtkLabel    *label)
{
    gtk_label_set_text (label, adw_tab_page_get_title (page));
}

static void
update_tab_strip_selection (NautilusWindow *window,
                            AdwTabView     *tab_view,
                            GtkBox         *tab_strip)
{
    AdwTabPage *selected_page = adw_tab_view_get_selected_page (tab_view);

    for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (tab_strip));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *button = g_object_get_data (G_OBJECT (child), "tab-toggle");
        AdwTabPage *page = g_object_get_data (G_OBJECT (child), "tab-page");

        if (page == NULL || !GTK_IS_TOGGLE_BUTTON (button))
        {
            continue;
        }

        gboolean selected = page == selected_page;
        g_object_set_data (G_OBJECT (button), "syncing-selection", GINT_TO_POINTER (1));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), selected);
        g_object_set_data (G_OBJECT (button), "syncing-selection", NULL);
        if (selected)
        {
            gtk_widget_add_css_class (child, "selected");
        }
        else
        {
            gtk_widget_remove_css_class (child, "selected");
        }
    }
}

static void
update_tab_strip (NautilusWindow *window,
                  AdwTabView     *tab_view,
                  GtkBox         *tab_strip)
{
    GtkWidget *tab_bar = tab_view == window->secondary_tab_view
                         ? window->secondary_tab_bar : window->main_tab_bar;
    int n_pages = adw_tab_view_get_n_pages (tab_view);
    int total_pages = n_pages +
                      adw_tab_view_get_n_pages (tab_view == window->secondary_tab_view
                                                ? window->tab_view
                                                : window->secondary_tab_view);
    gboolean inverted = is_layout_reversed ();

    /* Reveal both pane tab bars once the window has multiple pages. This keeps
     * a one-tab pane visible alongside a pane that has several tabs. */
    gtk_widget_set_visible (window->main_tab_bar, total_pages > 1);
    gtk_widget_set_visible (window->secondary_tab_bar,
                            total_pages > 1 && window->split_enabled);

    while (gtk_widget_get_first_child (GTK_WIDGET (tab_strip)) != NULL)
    {
        gtk_box_remove (tab_strip, gtk_widget_get_first_child (GTK_WIDGET (tab_strip)));
    }

    for (int i = 0; i < adw_tab_view_get_n_pages (tab_view); i++)
    {
        AdwTabPage *page = adw_tab_view_get_nth_page (tab_view, i);
        GtkWidget *chip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *toggle = gtk_toggle_button_new ();
        GtkWidget *label = gtk_label_new (adw_tab_page_get_title (page));
        GtkWidget *close_button = gtk_button_new_from_icon_name ("window-close-symbolic");
        GtkWidget *popover = gtk_popover_menu_new_from_model (adw_tab_view_get_menu_model (tab_view));
        GtkGesture *context_gesture = gtk_gesture_click_new ();
        GtkGesture *drag_gesture = gtk_gesture_drag_new ();
        TabItemData *data = tab_item_data_new (window, tab_view, tab_strip, tab_bar, page);
        TabItemData *close_data = tab_item_data_new (window, tab_view, tab_strip, tab_bar, page);

        data->chip = chip;
        g_object_set_data (G_OBJECT (chip), "tab-page", page);
        g_object_set_data (G_OBJECT (chip), "tab-toggle", toggle);
        g_object_set_data_full (G_OBJECT (chip), "tab-item-data", data, tab_item_data_free);
        g_object_set_data (G_OBJECT (chip), "tab-context-popover", popover);
        gtk_widget_add_css_class (chip, "vinder-tab");
        gtk_widget_add_css_class (toggle, "flat");
        gtk_widget_add_css_class (toggle, "vinder-tab-toggle");
        gtk_widget_add_css_class (close_button, "flat");
        gtk_widget_set_size_request (chip, 128, -1);
        gtk_widget_set_size_request (toggle, 100, -1);
        gtk_widget_set_size_request (close_button, 24, -1);
        gtk_widget_set_hexpand (chip, FALSE);
        gtk_widget_set_hexpand (toggle, FALSE);
        gtk_widget_set_hexpand (close_button, FALSE);
        gtk_widget_set_hexpand (label, FALSE);
        gtk_widget_set_tooltip_text (toggle, adw_tab_page_get_title (page));
        gtk_widget_set_tooltip_text (close_button, _("Close Tab"));
        gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign (GTK_LABEL (label), 0.0);
        gtk_label_set_max_width_chars (GTK_LABEL (label), 18);
        gtk_widget_set_margin_start (label, 9);
        gtk_widget_set_margin_end (label, 9);
        gtk_button_set_child (GTK_BUTTON (toggle), label);
        gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
        gtk_widget_set_parent (popover, chip);
        gtk_box_append (GTK_BOX (chip), inverted ? close_button : toggle);
        gtk_box_append (GTK_BOX (chip), inverted ? toggle : close_button);
        gtk_box_append (tab_strip, chip);

        g_signal_connect_object (page, "notify::title",
                                 G_CALLBACK (tab_page_title_changed_cb),
                                 G_OBJECT (label), 0);
        g_signal_connect (toggle, "toggled", G_CALLBACK (tab_item_select_cb), data);
        g_signal_connect (close_button, "clicked", G_CALLBACK (tab_item_close_cb), close_data);
        g_object_set_data_full (G_OBJECT (close_button), "tab-item-data",
                                close_data, tab_item_data_free);
        g_signal_connect (popover, "closed", G_CALLBACK (tab_item_menu_closed_cb), data);

        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (context_gesture), GDK_BUTTON_SECONDARY);
        g_signal_connect (context_gesture, "pressed",
                          G_CALLBACK (tab_item_context_pressed_cb), data);
        gtk_widget_add_controller (chip, GTK_EVENT_CONTROLLER (context_gesture));

        g_signal_connect (drag_gesture, "drag-begin",
                          G_CALLBACK (tab_item_drag_begin_cb), data);
        g_signal_connect (drag_gesture, "drag-end",
                          G_CALLBACK (tab_item_drag_end_cb), data);
        gtk_widget_add_controller (toggle, GTK_EVENT_CONTROLLER (drag_gesture));
    }

    update_tab_strip_selection (window, tab_view, tab_strip);
}

static void
tab_view_page_reordered_cb (AdwTabView     *tab_view,
                            AdwTabPage     *page,
                            int             position,
                            NautilusWindow *window)
{
    update_tab_strip (window, tab_view,
                      tab_view == window->secondary_tab_view
                      ? window->secondary_tab_strip : window->main_tab_strip);
}

static void
tab_view_notify_selected_page_cb (AdwTabView     *tab_view,
                                  GParamSpec     *pspec,
                                  NautilusWindow *window)
{
    AdwTabPage *page = adw_tab_view_get_selected_page (tab_view);
    NautilusWindowSlot *slot;

    update_tab_strip_selection (window, tab_view,
                                tab_view == window->secondary_tab_view
                                ? window->secondary_tab_strip : window->main_tab_strip);

    if (tab_view != get_active_tab_view (window) || page == NULL)
    {
        return;
    }

    slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    g_return_if_fail (slot != NULL);

    set_active_slot (window, slot);
}

static void
pane_focus_entered_cb (GtkEventControllerFocus *controller,
                       NautilusWindow          *window)
{
    GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

    if (widget == window->secondary_pane || widget == GTK_WIDGET (window->secondary_tab_bar))
    {
        set_active_pane (window, TRUE);
    }
    else
    {
        set_active_pane (window, FALSE);
    }
}

static void
pane_position_changed_cb (GtkPaned       *paned,
                          GParamSpec     *pspec,
                          NautilusWindow *window)
{
    int width = gtk_widget_get_width (GTK_WIDGET (paned));

    if (window->split_enabled && width > 0)
    {
        double fraction = (double) gtk_paned_get_position (paned) / width;
        g_settings_set_double (nautilus_window_state, "vinder-split-position",
                               CLAMP (fraction, 0.2, 0.8));
    }
}

static void
disconnect_slot (NautilusWindow     *window,
                 NautilusWindowSlot *slot)
{
    g_signal_handlers_disconnect_by_data (slot, window);
}

static void
connect_slot_to_window (NautilusWindow     *window,
                        NautilusWindowSlot *slot)
{
    g_signal_connect_swapped (slot, "notify::allow-stop",
                              G_CALLBACK (update_cursor), window);
    g_signal_connect (slot, "notify::location",
                      G_CALLBACK (on_slot_location_changed), window);
}

static void
on_update_page_tooltip (NautilusWindowSlot *slot,
                        GParamSpec         *pspec,
                        AdwTabPage         *page)
{
    GFile *location = nautilus_window_slot_get_location (slot);
    g_autofree char *escaped_name = NULL;

    if (location == NULL)
    {
        return;
    }

    /* Set the tooltip on the label's parent (the tab label hbox),
     * so it covers all of the tab label.
     */

    if (g_file_has_uri_scheme (location, SCHEME_SEARCH))
    {
        escaped_name = g_markup_escape_text (nautilus_window_slot_get_title (slot), -1);
    }
    else
    {
        g_autofree gchar *location_name = g_file_get_parse_name (location);
        escaped_name = g_markup_escape_text (location_name, -1);
    }

    adw_tab_page_set_tooltip (page, escaped_name);
}

static NautilusWindowSlot *
nautilus_window_create_and_init_slot_in_view (NautilusWindow *window,
                                               AdwTabView     *view)
{
    g_autoptr (NautilusWindowSlot) slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);

    AdwTabPage *current = adw_tab_view_get_selected_page (view);
    AdwTabPage *page = adw_tab_view_add_page (view, GTK_WIDGET (slot), current);

    g_object_bind_property (slot, "allow-stop",
                            page, "loading",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (slot, "title",
                            page, "title",
                            G_BINDING_SYNC_CREATE);
    g_signal_connect (slot, "notify::title", G_CALLBACK (on_update_page_tooltip), page);

    /* Assure that AdwTabView has added slot to slot list */
    g_warn_if_fail (g_list_find (window->slots, slot) != NULL);

    return slot;
}

static NautilusWindowSlot *
nautilus_window_create_and_init_slot (NautilusWindow *window)
{
    return nautilus_window_create_and_init_slot_in_view (window, get_active_tab_view (window));
}

static void
action_toggle_split_view (GSimpleAction *action,
                          GVariant      *value,
                          gpointer       user_data)
{
    toggle_split_view_internal (action, g_variant_get_boolean (value), TRUE,
                                NAUTILUS_WINDOW (user_data));
}

static void
toggle_split_view_internal (GSimpleAction *action,
                            gboolean       enabled,
                            gboolean       create_seed_slot,
                            NautilusWindow *window)
{
    if (enabled == window->split_enabled)
    {
        g_simple_action_set_state (action, g_variant_new_boolean (enabled));
        return;
    }

    if (enabled)
    {
        GFile *location = window->active_slot != NULL
                          ? nautilus_window_slot_get_location (window->active_slot) : NULL;
        g_autoptr (GFile) fallback_location = NULL;
        NautilusWindowSlot *slot;
        int width = gtk_widget_get_width (GTK_WIDGET (window->paned));

        window->split_enabled = TRUE;
        gtk_widget_set_visible (window->secondary_pane, TRUE);
        gtk_widget_set_visible (window->secondary_tab_bar_container, TRUE);
        if (width > 0)
        {
            double fraction = CLAMP (g_settings_get_double (nautilus_window_state,
                                                             "vinder-split-position"),
                                     0.2, 0.8);
            int position = (int) (width * fraction);
            position = CLAMP (position, 220, MAX (220, width - 220));
            gtk_paned_set_position (window->paned, position);
        }

        if (location == NULL || g_file_has_uri_scheme (location, SCHEME_SEARCH))
        {
            fallback_location = g_file_new_for_path (g_get_home_dir ());
            location = fallback_location;
        }

        slot = NULL;
        if (create_seed_slot)
        {
            slot = nautilus_window_create_and_init_slot_in_view (window,
                                                                 window->secondary_tab_view);
            nautilus_window_slot_open_location_full (slot, location, NULL);
        }
        set_active_pane (window, TRUE);
        if (slot != NULL)
        {
            gtk_widget_grab_focus (GTK_WIDGET (slot));
        }
    }
    else
    {
        AdwTabPage *selected_page = adw_tab_view_get_selected_page (window->secondary_tab_view);
        NautilusWindowSlot *selected_slot = selected_page != NULL
                                            ? NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (selected_page))
                                            : NULL;
        window->transferring_pages = TRUE;
        while (adw_tab_view_get_n_pages (window->secondary_tab_view) > 0)
        {
            AdwTabPage *page = adw_tab_view_get_nth_page (window->secondary_tab_view, 0);
            adw_tab_view_transfer_page (window->secondary_tab_view, page,
                                        window->tab_view,
                                        adw_tab_view_get_n_pages (window->tab_view));
        }
        window->transferring_pages = FALSE;
        window->split_enabled = FALSE;
        window->secondary_is_active = FALSE;
        gtk_widget_set_visible (window->secondary_pane, FALSE);
        gtk_widget_set_visible (window->secondary_tab_bar_container, FALSE);
        update_shared_tab_widgets (window);
        if (selected_slot != NULL)
        {
            AdwTabPage *page = adw_tab_view_get_page (window->tab_view, GTK_WIDGET (selected_slot));
            if (page != NULL)
            {
                adw_tab_view_set_selected_page (window->tab_view, page);
            }
        }
        set_active_pane (window, FALSE);
        if (window->active_slot != NULL)
        {
            gtk_widget_grab_focus (GTK_WIDGET (window->active_slot));
        }
    }

    update_tab_strip (window, window->tab_view, window->main_tab_strip);
    update_tab_strip (window, window->secondary_tab_view, window->secondary_tab_strip);

    g_simple_action_set_state (action, g_variant_new_boolean (enabled));
}

static void
action_focus_other_pane (GSimpleAction *action,
                         GVariant      *state,
                         gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    if (!window->split_enabled)
    {
        return;
    }

    set_active_pane (window, !window->secondary_is_active);
    if (window->active_slot != NULL)
    {
        gtk_widget_grab_focus (GTK_WIDGET (window->active_slot));
    }
}

void
nautilus_window_open_location_in_other_pane (NautilusWindow *window,
                                             GFile          *location)
{
    gboolean target_secondary;
    AdwTabView *target_view;
    AdwTabPage *page;
    NautilusWindowSlot *slot;

    g_return_if_fail (NAUTILUS_IS_WINDOW (window));
    g_return_if_fail (G_IS_FILE (location));

    if (!window->split_enabled)
    {
        toggle_split_view_internal (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (window),
                                                                                 "toggle-split-view")),
                                    TRUE, FALSE, window);
        target_secondary = TRUE;
    }
    else
    {
        target_secondary = !window->secondary_is_active;
    }

    set_active_pane (window, target_secondary);
    target_view = get_active_tab_view (window);
    page = adw_tab_view_get_selected_page (target_view);
    if (page != NULL)
    {
        slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
    }
    else
    {
        slot = nautilus_window_create_and_init_slot_in_view (window, target_view);
    }
    nautilus_window_slot_open_location_full (slot, location, NULL);
    gtk_widget_grab_focus (GTK_WIDGET (slot));
}

void
nautilus_window_sync_folder_view_mode (NautilusWindow     *window,
                                       NautilusWindowSlot *source_slot,
                                       GFile              *location,
                                       const char         *mode)
{
    gboolean grid;
    guint view_id;

    g_return_if_fail (NAUTILUS_IS_WINDOW (window));
    g_return_if_fail (NAUTILUS_IS_WINDOW_SLOT (source_slot));
    g_return_if_fail (G_IS_FILE (location));

    grid = g_str_equal (mode, "grid");
    view_id = grid ? NAUTILUS_VIEW_GRID_ID : NAUTILUS_VIEW_LIST_ID;

    for (GList *l = window->slots; l != NULL; l = l->next)
    {
        NautilusWindowSlot *slot = l->data;
        GFile *slot_location;
        NautilusFilesView *current_view;
        gboolean reload_list;

        if (slot == source_slot)
        {
            continue;
        }

        slot_location = nautilus_window_slot_get_location (slot);
        if (slot_location == NULL || !g_file_equal (slot_location, location))
        {
            continue;
        }

        current_view = nautilus_window_slot_get_current_view (slot);
        reload_list = !grid && current_view != NULL &&
                      nautilus_files_view_get_view_id (current_view) == NAUTILUS_VIEW_LIST_ID;
        gtk_widget_activate_action (GTK_WIDGET (slot), "slot.files-view-mode", "u", view_id);
        if (reload_list)
        {
            gtk_widget_activate_action (GTK_WIDGET (slot), "slot.reload", NULL);
        }
    }
}

static void
action_activate_split_view (GSimpleAction *action,
                            GVariant      *state,
                            gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    action_toggle_split_view (action, g_variant_new_boolean (!window->split_enabled), window);
}

static void
action_move_tab_to_other_pane (GSimpleAction *action,
                               GVariant      *state,
                               gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    AdwTabPage *page = get_current_page (window);
    NautilusWindowSlot *slot;
    AdwTabView *source;
    AdwTabView *target;

    if (page == NULL)
    {
        return;
    }

    slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
    source = get_tab_view_for_slot (window, slot);
    if (!window->split_enabled)
    {
        GFile *current_location = nautilus_window_slot_get_location (slot);
        g_autoptr (GFile) location_copy = current_location != NULL
                                         ? g_object_ref (current_location) : NULL;
        if (location_copy == NULL)
        {
            return;
        }

        toggle_split_view_internal (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (window),
                                                                                 "toggle-split-view")),
                                    TRUE, FALSE, window);

        NautilusWindowSlot *replacement = nautilus_window_create_and_init_slot_in_view (window,
                                                                                         window->tab_view);
        nautilus_window_slot_open_location_full (replacement, location_copy, NULL);
    }

    target = source == window->tab_view ? window->secondary_tab_view : window->tab_view;
    window->transferring_pages = TRUE;
    adw_tab_view_transfer_page (source, page, target, adw_tab_view_get_n_pages (target));
    window->transferring_pages = FALSE;
    set_active_pane (window, target == window->secondary_tab_view);
    adw_tab_view_set_selected_page (target, page);
    gtk_widget_grab_focus (GTK_WIDGET (slot));

    if (window->split_enabled && source == window->secondary_tab_view &&
        adw_tab_view_get_n_pages (source) == 0)
    {
        action_toggle_split_view (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (window),
                                                                               "toggle-split-view")),
                                  g_variant_new_boolean (FALSE), window);
    }
}

static NautilusWindowSlot *
get_slot_with_open_location (NautilusWindow *self,
                             GFile          *location)
{
    for (GList *l = self->slots; l != NULL; l = l->next)
    {
        NautilusWindowSlot *slot = l->data;
        GFile *slot_location = nautilus_window_slot_get_location (slot);

        if (slot_location != NULL &&
            g_file_equal (location, slot_location))
        {
            return slot;
        }
    }

    return NULL;
}

void
nautilus_window_open_location_full (NautilusWindow    *window,
                                    GFile             *location,
                                    NautilusOpenFlags  flags,
                                    NautilusFileList  *selection)
{
    /* Assert that we are not managing new windows */
    g_warn_if_fail ((flags & NAUTILUS_OPEN_FLAG_NEW_WINDOW) == 0);

    NautilusWindowSlot *target_slot = (flags & NAUTILUS_OPEN_FLAG_REUSE_EXISTING) != 0
                                      ? get_slot_with_open_location (window, location)
                                      : window->active_slot;

    if (target_slot == NULL || (flags & NAUTILUS_OPEN_FLAG_NEW_TAB) != 0)
    {
        target_slot = nautilus_window_create_and_init_slot (window);
    }

    /* Make the opened location the one active if we weren't asked for the
     * opposite, since it's the most usual use case */
    if ((flags & NAUTILUS_OPEN_FLAG_DONT_MAKE_ACTIVE) == 0)
    {
        gtk_window_present (GTK_WINDOW (window));
        set_active_slot (window, target_slot);
    }

    nautilus_window_slot_open_location_full (target_slot, location, selection);
}

static gboolean
nautilus_window_grab_focus (GtkWidget *widget)
{
    NautilusWindow *self = NAUTILUS_WINDOW (widget);

    if (self->active_slot != NULL)
    {
        return gtk_widget_grab_focus (GTK_WIDGET (self->active_slot));
    }

    return GTK_WIDGET_CLASS (nautilus_window_parent_class)->grab_focus (widget);
}

static void
remove_slot_from_window (NautilusWindowSlot *slot,
                         NautilusWindow     *window)
{
    g_return_if_fail (NAUTILUS_IS_WINDOW_SLOT (slot));
    g_return_if_fail (NAUTILUS_IS_WINDOW (window));

    g_debug ("Removing slot %p", slot);

    /* This also covers page detaches for tab transfers: the slot may already
     * have been unparented, so deactivate it before losing the window link. */
    if (window->active_slot == slot)
    {
        set_active_slot (window, NULL);
    }

    disconnect_slot (window, slot);
    window->slots = g_list_remove (window->slots, slot);
    g_signal_emit (window, signals[LOCATIONS_CHANGED], 0);
}

void
nautilus_window_new_tab (NautilusWindow *window)
{
    NautilusWindowSlot *current_slot;
    AdwTabPage *page;
    GFile *location;

    page = get_current_page (window);
    if (page == NULL)
    {
        return;
    }
    current_slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
    location = nautilus_window_slot_get_location (current_slot);

    if (location != NULL)
    {
        if (g_file_has_uri_scheme (location, SCHEME_SEARCH))
        {
            location = g_file_new_for_path (g_get_home_dir ());
        }
        else
        {
            g_object_ref (location);
        }

        nautilus_window_open_location_full (window, location,
                                            NAUTILUS_OPEN_FLAG_NEW_TAB,
                                            NULL);
        g_object_unref (location);
    }
}

static void
update_cursor (NautilusWindow *window)
{
    if (!gtk_widget_get_realized (GTK_WIDGET (window)))
    {
        return;
    }

    if (window->active_slot != NULL &&
        nautilus_window_slot_get_allow_stop (window->active_slot))
    {
        gtk_widget_set_cursor_from_name (GTK_WIDGET (window), "progress");
    }
    else
    {
        gtk_widget_set_cursor (GTK_WIDGET (window), NULL);
    }
}

/* Callback used when the places sidebar needs to know the drag action to suggest */
static GdkDragAction
places_sidebar_drag_action_requested_cb (NautilusSidebar *sidebar,
                                         NautilusFile    *dest_file,
                                         GList           *source_file_list)
{
    return nautilus_dnd_get_preferred_action (dest_file, source_file_list->data);
}
#if 0 && NAUTILUS_DND_NEEDS_GTK4_REIMPLEMENTATION
/* Callback used when the places sidebar needs us to pop up a menu with possible drag actions */
static GdkDragAction
places_sidebar_drag_action_ask_cb (NautilusSidebar *sidebar,
                                   GdkDragAction    actions,
                                   gpointer         user_data)
{
    return nautilus_drag_drop_action_ask (GTK_WIDGET (sidebar), actions);
}
#endif
static GList *
build_uri_list_from_gfile_list (GSList *file_list)
{
    GList *result;
    GSList *l;

    result = NULL;

    for (l = file_list; l; l = l->next)
    {
        GFile *file = l->data;
        char *uri;

        uri = g_file_get_uri (file);
        result = g_list_prepend (result, uri);
    }

    return g_list_reverse (result);
}

/* Callback used when the places sidebar has URIs dropped into it.  We do a normal file operation for them. */
static void
places_sidebar_drag_perform_drop_cb (NautilusSidebar *sidebar,
                                     GFile           *dest_file,
                                     GSList          *source_file_list,
                                     GdkDragAction    action,
                                     gpointer         user_data)
{
    char *dest_uri;
    GList *source_uri_list;

    dest_uri = g_file_get_uri (dest_file);
    source_uri_list = build_uri_list_from_gfile_list (source_file_list);

    nautilus_file_operations_copy_move (source_uri_list, dest_uri, action, GTK_WIDGET (sidebar), NULL, NULL, NULL);

    g_free (dest_uri);
    g_list_free_full (source_uri_list, g_free);
}

static void
action_restore_tab (GSimpleAction *action,
                    GVariant      *state,
                    gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    NautilusWindowSlot *slot;
    NautilusNavigationState *data;

    if (g_queue_get_length (window->tab_data_queue) == 0)
    {
        return;
    }

    data = g_queue_pop_head (window->tab_data_queue);

    slot = nautilus_window_create_and_init_slot (window);

    nautilus_window_slot_restore_navigation_state (slot, data);

    free_navigation_state (data);

    g_simple_action_set_enabled (action, g_queue_get_length (window->tab_data_queue) > 0);
}

static void
action_toggle_sidebar (GSimpleAction *action,
                       GVariant      *state,
                       gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    gboolean revealed;

    revealed = adw_overlay_split_view_get_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (window->split_view));
    adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (window->split_view), !revealed);
}

static void
nautilus_window_set_up_sidebar (NautilusWindow *window)
{
    nautilus_sidebar_set_open_flags (window->places_sidebar,
                                     (NAUTILUS_OPEN_FLAG_NORMAL
                                      | NAUTILUS_OPEN_FLAG_NEW_TAB
                                      | NAUTILUS_OPEN_FLAG_NEW_WINDOW
                                      | NAUTILUS_OPEN_FLAG_OTHER_PANE));

    g_signal_connect (window->places_sidebar, "drag-action-requested",
                      G_CALLBACK (places_sidebar_drag_action_requested_cb), window);
#if 0 && NAUTILUS_DND_NEEDS_GTK4_REIMPLEMENTATION
    g_signal_connect (window->places_sidebar, "drag-action-ask",
                      G_CALLBACK (places_sidebar_drag_action_ask_cb), window);
  #endif
    g_signal_connect (window->places_sidebar, "drag-perform-drop",
                      G_CALLBACK (places_sidebar_drag_perform_drop_cb), window);
}

static void
window_slot_close (NautilusWindow     *window,
                   NautilusWindowSlot *slot)
{
    AdwTabPage *page;
    AdwTabView *view;

    g_debug ("Requesting to remove slot %p from window %p", slot, window);
    if (window == NULL || slot == NULL)
    {
        return;
    }

    view = get_tab_view_for_slot (window, slot);
    if (view == NULL)
    {
        return;
    }

    page = adw_tab_view_get_page (view, GTK_WIDGET (slot));
    if (page != NULL)
    {
        adw_tab_view_close_page (view, page);
    }
}

static void
nautilus_window_sync_location_widgets (NautilusWindow *window)
{
    if (window->active_slot == NULL)
    {
        gtk_widget_set_visible (window->network_address_bar, FALSE);
        return;
    }

    GFile *location = nautilus_window_slot_get_location (window->active_slot);

    if (location != NULL)
    {
        gtk_widget_set_visible (window->network_address_bar,
                                g_file_has_uri_scheme (location, SCHEME_NETWORK_VIEW));
    }
}

static gchar *
toast_undo_deleted_get_label (NautilusFileUndoInfo *undo_info)
{
    g_autoptr (GList) files = nautilus_file_undo_info_trash_get_files (NAUTILUS_FILE_UNDO_INFO_TRASH (undo_info));
    gchar *label;
    gint length;

    length = g_list_length (files);
    if (length == 1)
    {
        g_autofree gchar *basename = g_file_get_basename (files->data);
        g_autofree gchar *file_label = g_filename_display_name (basename);

        /* Translators: only one item has been moved to trash and %s is its name. */
        label = g_markup_printf_escaped (_("“%s” moved to trash"), file_label);
    }
    else
    {
        /* Translators: one or more items might have been moved to trash, and %d
         * is the count. */
        label = g_markup_printf_escaped (ngettext ("%d file moved to trash", "%d files moved to trash", length), length);
    }

    return label;
}

static gchar *
toast_undo_unstar_get_label (NautilusFileUndoInfo *undo_info)
{
    GList *files;
    gchar *label;
    gint length;

    files = nautilus_file_undo_info_starred_get_files (NAUTILUS_FILE_UNDO_INFO_STARRED (undo_info));
    length = g_list_length (files);
    if (length == 1)
    {
        const char *file_label = nautilus_file_get_display_name (files->data);
        /* Translators: one item has been unstarred and %s is its name. */
        label = g_markup_printf_escaped (_("“%s” unstarred"), file_label);
    }
    else
    {
        /* Translators: one or more items have been unstarred, and %d
         * is the count. */
        label = g_markup_printf_escaped (ngettext ("%d file unstarred", "%d files unstarred", length), length);
    }

    return label;
}

static void
update_undo_redo_menu_items (NautilusWindow               *window,
                             NautilusFileUndoInfo         *info,
                             NautilusFileUndoManagerState  undo_state)
{
    gboolean undo_active;
    gboolean redo_active;
    g_autofree gchar *undo_label = NULL;
    g_autofree gchar *redo_label = NULL;
    g_autofree gchar *undo_description = NULL;
    g_autofree gchar *redo_description = NULL;
    gboolean is_undo;
    g_autoptr (GMenu) updated_section = g_menu_new ();
    GAction *action;

    /* Look at the last action from the undo manager, and get the text that
     * describes it, e.g. "Undo Create Folder"/"Redo Create Folder"
     */
    undo_active = redo_active = FALSE;
    if (info != NULL && undo_state > NAUTILUS_FILE_UNDO_MANAGER_STATE_NONE)
    {
        is_undo = undo_state == NAUTILUS_FILE_UNDO_MANAGER_STATE_UNDO;

        /* The last action can either be undone/redone. Activate the corresponding
         * menu item and deactivate the other
         */
        undo_active = is_undo;
        redo_active = !is_undo;
        nautilus_file_undo_info_get_strings (info, &undo_label, &undo_description,
                                             &redo_label, &redo_description);
    }

    /* Set the label of the undo and redo menu items, and activate them appropriately
     */
    if (!undo_active || undo_label == NULL)
    {
        g_set_str (&undo_label, _("_Undo"));
    }
    g_menu_append (updated_section, undo_label, "win.undo");
    action = g_action_map_lookup_action (G_ACTION_MAP (window), "undo");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), undo_active);

    if (!redo_active || redo_label == NULL)
    {
        g_set_str (&redo_label, _("_Redo"));
    }
    g_menu_append (updated_section, redo_label, "win.redo");
    action = g_action_map_lookup_action (G_ACTION_MAP (window), "redo");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), redo_active);

    nautilus_gmenu_set_from_model (G_MENU (window->undo_redo_section),
                                   G_MENU_MODEL (updated_section));
}

static void
nautilus_window_on_undo_changed (NautilusFileUndoManager *manager,
                                 NautilusWindow          *window)
{
    NautilusFileUndoInfo *undo_info;
    NautilusFileUndoManagerState state;
    AdwToast *toast;

    undo_info = nautilus_file_undo_manager_get_action ();
    state = nautilus_file_undo_manager_get_state ();

    update_undo_redo_menu_items (window, undo_info, state);

    if (undo_info != NULL &&
        state == NAUTILUS_FILE_UNDO_MANAGER_STATE_UNDO)
    {
        gboolean popup_toast = FALSE;
        g_autofree gchar *label = NULL;

        if (nautilus_file_undo_info_get_op_type (undo_info) == NAUTILUS_FILE_UNDO_OP_MOVE_TO_TRASH)
        {
            g_autoptr (GList) files = NULL;

            files = nautilus_file_undo_info_trash_get_files (NAUTILUS_FILE_UNDO_INFO_TRASH (undo_info));

            /* Don't pop up a notification if user canceled the operation or the focus
             * is not in the this window. This is an easy way to know from which window
             * was the delete operation made */
            if (files != NULL && gtk_window_is_active (GTK_WINDOW (window)))
            {
                popup_toast = TRUE;
                label = toast_undo_deleted_get_label (undo_info);
            }
        }
        else if (nautilus_file_undo_info_get_op_type (undo_info) == NAUTILUS_FILE_UNDO_OP_STARRED)
        {
            if (window->active_slot == NULL)
            {
                return;
            }

            GFile *location = nautilus_window_slot_get_location (window->active_slot);
            /* Don't pop up a notification if the focus is not in the this
             * window. This is an easy way to know from which window was the
             * unstart operation made */
            if (location != NULL &&
                g_file_has_uri_scheme (location, SCHEME_STARRED) &&
                gtk_window_is_active (GTK_WINDOW (window)) &&
                !nautilus_file_undo_info_starred_is_starred (NAUTILUS_FILE_UNDO_INFO_STARRED (undo_info)))
            {
                popup_toast = TRUE;
                label = toast_undo_unstar_get_label (undo_info);
            }
        }

        if (window->last_undo_toast != NULL)
        {
            adw_toast_dismiss (window->last_undo_toast);
            g_clear_weak_pointer (&window->last_undo_toast);
        }

        if (popup_toast)
        {
            toast = adw_toast_new (label);
            adw_toast_set_button_label (toast, _("Undo"));
            adw_toast_set_action_name (toast, "win.undo");
            adw_toast_set_priority (toast, ADW_TOAST_PRIORITY_HIGH);
            g_set_weak_pointer (&window->last_undo_toast, toast);
            adw_toast_overlay_add_toast (window->toast_overlay, toast);
        }
    }
}

void
nautilus_window_show_operation_notification (NautilusWindow *window,
                                             gchar          *main_label,
                                             GFile          *folder_to_open,
                                             gboolean        was_quick)
{
    gboolean is_current_location;
    AdwToast *toast;

    if (window->active_slot == NULL || !gtk_window_is_active (GTK_WINDOW (window)))
    {
        return;
    }

    is_current_location = g_file_equal (folder_to_open,
                                        nautilus_window_slot_get_location (window->active_slot));

    if (is_current_location && was_quick)
    {
        return;
    }

    toast = adw_toast_new (main_label);
    adw_toast_set_priority (toast, ADW_TOAST_PRIORITY_HIGH);
    adw_toast_set_use_markup (toast, FALSE);

    if (!is_current_location)
    {
        GVariant *target;

        target = g_variant_new_take_string (g_file_get_uri (folder_to_open));

        adw_toast_set_button_label (toast, C_("toast button", "Open Folder"));
        adw_toast_set_action_name (toast, "slot.open-location");
        adw_toast_set_action_target_value (toast, target);
    }

    adw_toast_overlay_add_toast (window->toast_overlay, toast);
}

static gboolean
tab_view_close_page_cb (AdwTabView     *view,
                        AdwTabPage     *page,
                        NautilusWindow *window)
{
    NautilusWindowSlot *slot;
    NautilusNavigationState *data;
    GAction *restore_action;

    slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    /* A seeded tab used by "Move Tab to Other Pane" is already untracked. */
    if (!g_list_find (window->slots, slot))
    {
        return GDK_EVENT_PROPAGATE;
    }

    if (adw_tab_page_get_pinned (page))
    {
        adw_tab_view_close_page_finish (view, page, FALSE);
        return GDK_EVENT_STOP;
    }

    data = nautilus_window_slot_get_navigation_state (slot);
    if (data != NULL)
    {
        g_queue_push_head (window->tab_data_queue, data);
    }

    nautilus_window_slot_prepare_for_close (slot);
    remove_slot_from_window (slot, window);
    window->closing_page = TRUE;
    adw_tab_view_close_page_finish (view, page, TRUE);
    window->closing_page = FALSE;

    if (window->slots == NULL)
    {
        g_debug ("Last slot removed, closing the window");
        nautilus_window_close (window);
    }
    else
    {
        restore_action = g_action_map_lookup_action (G_ACTION_MAP (window), "restore-tab");
        g_simple_action_set_enabled (G_SIMPLE_ACTION (restore_action),
                                     g_queue_get_length (window->tab_data_queue) > 0);
    }

#ifdef HAVE_MALLOC_TRIM
    schedule_trim ();
#endif

    return GDK_EVENT_STOP;
}

static void
collapse_empty_pane_if_needed (NautilusWindow *window)
{
    GAction *action;

    if (!window->split_enabled || window->slots == NULL ||
        (adw_tab_view_get_n_pages (window->tab_view) > 0 &&
         adw_tab_view_get_n_pages (window->secondary_tab_view) > 0))
    {
        return;
    }

    action = g_action_map_lookup_action (G_ACTION_MAP (window), "toggle-split-view");
    action_toggle_split_view (G_SIMPLE_ACTION (action), g_variant_new_boolean (FALSE), window);
}

static gboolean
collapse_empty_pane_when_transfer_done (gpointer user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    if (window->closing || window->tab_view == NULL || window->secondary_tab_view == NULL)
    {
        window->collapse_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (adw_tab_view_get_is_transferring_page (window->tab_view) ||
        adw_tab_view_get_is_transferring_page (window->secondary_tab_view))
    {
        return G_SOURCE_CONTINUE;
    }

    window->collapse_source_id = 0;
    collapse_empty_pane_if_needed (window);

    return G_SOURCE_REMOVE;
}

static gboolean
collapse_empty_pane_idle (gpointer user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    window->collapse_source_id = 0;
    if (window->closing || window->tab_view == NULL || window->secondary_tab_view == NULL)
    {
        return G_SOURCE_REMOVE;
    }

    if (adw_tab_view_get_is_transferring_page (window->tab_view) ||
        adw_tab_view_get_is_transferring_page (window->secondary_tab_view))
    {
        window->collapse_source_id =
            g_timeout_add_full (G_PRIORITY_DEFAULT, 50,
                               collapse_empty_pane_when_transfer_done,
                               g_object_ref (window), g_object_unref);
        return G_SOURCE_REMOVE;
    }

    collapse_empty_pane_if_needed (window);
    return G_SOURCE_REMOVE;
}

static void
tab_view_page_detached_cb (AdwTabView     *tab_view,
                           AdwTabPage     *page,
                           gint            position,
                           NautilusWindow *window)
{
    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    if (window->menu_page == page)
    {
        window->menu_page = NULL;
    }

    if (!window->transferring_pages && g_list_find (window->slots, slot))
    {
        remove_slot_from_window (slot, window);
    }

    if (!window->closing && !window->transferring_pages && window->split_enabled &&
        adw_tab_view_get_n_pages (tab_view) == 0)
    {
        /* Tab DnD emits page-detached before page-attached. Defer pane collapse
         * until the transfer has landed, or we can hide the destination pane. */
        if (window->closing_page)
        {
            if (window->collapse_source_id != 0)
            {
                g_clear_handle_id (&window->collapse_source_id, g_source_remove);
            }
            window->collapse_source_id =
                g_idle_add_full (G_PRIORITY_HIGH_IDLE, collapse_empty_pane_idle,
                                 g_object_ref (window), g_object_unref);
        }
        else
        {
            if (window->collapse_source_id == 0)
            {
                window->collapse_source_id =
                    g_timeout_add_full (G_PRIORITY_DEFAULT, 50,
                                       collapse_empty_pane_when_transfer_done,
                                       g_object_ref (window), g_object_unref);
            }
        }
    }

    update_tab_strip (window, tab_view,
                      tab_view == window->secondary_tab_view
                      ? window->secondary_tab_strip : window->main_tab_strip);
}

static void
tab_view_page_attached_cb (AdwTabView     *tab_view,
                           AdwTabPage     *page,
                           gint            position,
                           NautilusWindow *window)
{
    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    if (!window->transferring_pages && !g_list_find (window->slots, slot))
    {
        connect_slot_to_window (window, slot);
        window->slots = g_list_prepend (window->slots, slot);
    }

    if (!window->transferring_pages && window->split_enabled)
    {
        set_active_pane (window, tab_view == window->secondary_tab_view);
    }

    update_tab_strip (window, tab_view,
                      tab_view == window->secondary_tab_view
                      ? window->secondary_tab_strip : window->main_tab_strip);

    g_signal_emit (window, signals[LOCATIONS_CHANGED], 0);
}

static AdwTabView *
tab_view_create_window_cb (AdwTabView     *tab_view,
                           NautilusWindow *window)
{
    NautilusApplication *app;
    NautilusWindow *new_window;

    app = NAUTILUS_APPLICATION (g_application_get_default ());
    new_window = nautilus_application_create_window (app);
    gtk_window_set_display (GTK_WINDOW (new_window),
                            gtk_widget_get_display (GTK_WIDGET (tab_view)));

    gtk_window_present (GTK_WINDOW (new_window));

    return new_window->tab_view;
}

static void
action_tab_move_new_window (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);
    AdwTabView *source = get_tab_view_for_slot (window,
                                                 NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page)));
    AdwTabView *new_view = tab_view_create_window_cb (source, window);

    adw_tab_view_transfer_page (source, page, new_view, 0);
}

static const GActionEntry win_entries[] =
{
    { .name = "current-location-menu", .activate = action_show_current_location_menu },
    { .name = "new-tab", .activate = action_new_tab },
    { .name = "undo", .activate = action_undo },
    { .name = "redo", .activate = action_redo },
    { .name = "open-tab-overview", .activate = action_open_tab_overview },
    /* Only accessible by shortcuts */
    { .name = "close-current-view", .activate = action_close_current_view },
    { .name = "close-other-tabs", .activate = action_close_other_tabs },
    { .name = "go-home", .activate = action_go_home },
    { .name = "tab-move-left", .activate = action_tab_move_left },
    { .name = "tab-move-right", .activate = action_tab_move_right },
    { .name = "tab-move-new-window", .activate = action_tab_move_new_window },
    { .name = "go-to-tab", .parameter_type = "i", .state = "0", .change_state = action_go_to_tab },
    { .name = "restore-tab", .activate = action_restore_tab },
    { .name = "toggle-sidebar", .activate = action_toggle_sidebar },
    { .name = "toggle-split-view", .activate = action_activate_split_view,
      .state = "false", .change_state = action_toggle_split_view },
    { .name = "focus-other-pane", .activate = action_focus_other_pane },
    { .name = "tab-move-other-pane", .activate = action_move_tab_to_other_pane },
};

static void
update_files_header_blur (NautilusWindow *window)
{
    gboolean enabled = g_settings_get_boolean (nautilus_preferences, "vinder-blur") &&
                       g_settings_get_boolean (nautilus_preferences, "vinder-scroll-animation") &&
                       !adw_style_manager_get_high_contrast (adw_style_manager_get_default ());

    if (enabled)
    {
        gtk_widget_add_css_class (GTK_WIDGET (window->files_toolbar_view), "scroll-animation");
    }
    else
    {
        gtk_widget_remove_css_class (GTK_WIDGET (window->files_toolbar_view), "scroll-animation");
    }
    adw_toolbar_view_set_extend_content_to_top_edge (window->files_toolbar_view, enabled);
}

/* The blur stylesheet leaves room for the toolbar, so it needs to know how tall it really is. */
static void
report_top_bar_height (NautilusWindow *window)
{
    vinder_interface_set_top_bar_height (NAUTILUS_APPLICATION (g_application_get_default ()),
                                         adw_toolbar_view_get_top_bar_height (window->files_toolbar_view));
}

static void
update_zebra_rows (NautilusWindow *window)
{
    GtkWidget *view = GTK_WIDGET (window->files_toolbar_view);

    if (g_settings_get_boolean (nautilus_preferences, "vinder-zebra-rows"))
    {
        gtk_widget_add_css_class (view, "zebra-rows");
    }
    else
    {
        gtk_widget_remove_css_class (view, "zebra-rows");
    }
}

static void
update_file_icon_shadows (NautilusWindow *window)
{
    GtkWidget *files_toolbar_view = GTK_WIDGET (window->files_toolbar_view);

    if (g_settings_get_boolean (nautilus_preferences, "vinder-pop-icons"))
    {
        gtk_widget_add_css_class (files_toolbar_view, "pop-icons");
    }
    else
    {
        gtk_widget_remove_css_class (files_toolbar_view, "pop-icons");
    }
}

static void
nautilus_window_initialize_actions (NautilusWindow *window)
{
    GApplication *app;
    GAction *action;
    gchar detailed_action[80];
    gchar accel[80];
    gint i;

    g_action_map_add_action_entries (G_ACTION_MAP (window),
                                     win_entries, G_N_ELEMENTS (win_entries),
                                     window);

    app = g_application_get_default ();
    nautilus_application_set_accelerator (app, "win.new-tab", "<control>t");
    nautilus_application_set_accelerator (app, "win.open-tab-overview", "<shift><control>o");
    nautilus_application_set_accelerator (app, "win.close-current-view", "<control>w");

    nautilus_application_set_accelerator (app, "win.undo", "<control>z");
    nautilus_application_set_accelerator (app, "win.redo", "<shift><control>z");
    /* Only accessible by shortcuts */
    nautilus_application_set_accelerator (app, "win.tab-move-left", "<shift><control>Page_Up");
    nautilus_application_set_accelerator (app, "win.tab-move-right", "<shift><control>Page_Down");
    nautilus_application_set_accelerator (app, "win.current-location-menu", "F10");
    nautilus_application_set_accelerator (app, "win.restore-tab", "<shift><control>t");
    nautilus_application_set_accelerator (app, "win.toggle-sidebar", "F9");
    nautilus_application_set_accelerator (app, "win.toggle-split-view", "F3");
    nautilus_application_set_accelerator (app, "win.focus-other-pane", "F6");

    /* Alt+N for the first 9 tabs */
    for (i = 0; i < 9; ++i)
    {
        g_snprintf (detailed_action, sizeof (detailed_action), "win.go-to-tab(%i)", i);
        g_snprintf (accel, sizeof (accel), "<alt>%i", i + 1);
        nautilus_application_set_accelerator (app, detailed_action, accel);
    }

    nautilus_window_on_undo_changed (nautilus_file_undo_manager_get (), window);

    action = g_action_map_lookup_action (G_ACTION_MAP (window), "toggle-sidebar");
    g_object_bind_property (window->split_view, "collapsed",
                            action, "enabled", G_BINDING_SYNC_CREATE);

    action = g_action_map_lookup_action (G_ACTION_MAP (window), "restore-tab");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);
}

static gboolean
is_layout_reversed (void)
{
    GtkSettings *settings = gtk_settings_get_default ();
    g_autofree char *layout = NULL;
    g_auto (GStrv) parts = NULL;

    if (settings != NULL)
    {
        g_object_get (settings, "gtk-decoration-layout", &layout, NULL);
    }

    if (layout != NULL)
    {
        parts = g_strsplit (layout, ":", 2);
    }

    /* Invalid layout, don't even try */
    g_return_val_if_fail (parts != NULL && g_strv_length (parts) >= 2, FALSE);

    return (g_strrstr (parts[0], "close") != NULL);
}

static void
notify_decoration_layout_cb (NautilusWindow *self)
{
    update_tab_strip (self, self->tab_view, self->main_tab_strip);
    update_tab_strip (self, self->secondary_tab_view, self->secondary_tab_strip);
}

static void
nautilus_window_constructed (GObject *self)
{
    NautilusWindow *window;
    NautilusApplication *application;
    GtkSettings *settings;

    window = NAUTILUS_WINDOW (self);

    G_OBJECT_CLASS (nautilus_window_parent_class)->constructed (self);

    application = NAUTILUS_APPLICATION (g_application_get_default ());
    gtk_window_set_application (GTK_WINDOW (window), GTK_APPLICATION (application));

    settings = gtk_settings_get_default ();
    g_signal_connect_object (settings, "notify::gtk-decoration-layout",
                             G_CALLBACK (notify_decoration_layout_cb), window,
                             G_CONNECT_SWAPPED);
    notify_decoration_layout_cb (window);

    nautilus_window_set_up_sidebar (window);


    g_signal_connect_object (nautilus_file_undo_manager_get (), "undo-changed",
                             G_CALLBACK (nautilus_window_on_undo_changed), self,
                             G_CONNECT_AFTER);

    /* Is required that the UI is constructed before initializing the actions, since
     * some actions trigger UI widgets to show/hide. */
    nautilus_window_initialize_actions (window);
}

static void
nautilus_window_dispose (GObject *object)
{
    NautilusWindow *window;
    GList *slots_copy;

    window = NAUTILUS_WINDOW (object);

    g_debug ("Destroying window");
    window->closing = TRUE;
    g_clear_handle_id (&window->collapse_source_id, g_source_remove);

    /* close all slots safely */
    slots_copy = g_list_copy (window->slots);
    for (GList *l = slots_copy; l != NULL; l = l->next)
    {
        nautilus_window_slot_prepare_for_close (l->data);
    }
    g_list_foreach (slots_copy, (GFunc) remove_slot_from_window, window);
    g_list_free (slots_copy);

    /* the slots list should now be empty */
    g_warn_if_fail (window->slots == NULL);

    g_clear_weak_pointer (&window->active_slot);

    gtk_widget_dispose_template (GTK_WIDGET (window), NAUTILUS_TYPE_WINDOW);

    G_OBJECT_CLASS (nautilus_window_parent_class)->dispose (object);
}

static void
nautilus_window_finalize (GObject *object)
{
    NautilusWindow *window;

    window = NAUTILUS_WINDOW (object);

    g_signal_handlers_disconnect_by_func (nautilus_file_undo_manager_get (),
                                          G_CALLBACK (nautilus_window_on_undo_changed),
                                          window);

    g_queue_free_full (window->tab_data_queue, free_navigation_state);

    /* nautilus_window_close() should have run */
    g_warn_if_fail (window->slots == NULL);

    G_OBJECT_CLASS (nautilus_window_parent_class)->finalize (object);
}

static void
nautilus_window_save_geometry (NautilusWindow *window)
{
    gint width;
    gint height;
    GVariant *initial_size;

    gtk_window_get_default_size (GTK_WINDOW (window), &width, &height);
    initial_size = g_variant_new_parsed ("(%i, %i)", width, height);

    g_settings_set_value (nautilus_window_state,
                          NAUTILUS_WINDOW_STATE_INITIAL_SIZE,
                          initial_size);

    if (window->split_enabled)
    {
        width = gtk_widget_get_width (GTK_WIDGET (window->paned));
        if (width > 0)
        {
            double fraction = (double) gtk_paned_get_position (window->paned) / width;
            g_settings_set_double (nautilus_window_state, "vinder-split-position",
                                   CLAMP (fraction, 0.2, 0.8));
        }
    }
}

void
nautilus_window_close (NautilusWindow *window)
{
    g_return_if_fail (NAUTILUS_IS_WINDOW (window));

    window->closing = TRUE;
    g_clear_handle_id (&window->collapse_source_id, g_source_remove);

    nautilus_window_save_geometry (window);
    set_active_slot (window, NULL);

    /* The pad controller hold a reference to the window, creating a cycle.
     * Usually, reference cycles are resolved in dispose(), but GTK removes the
     * controllers in finalize(), so our only option is to manually remove it
     * here before starting the destruction of the window. */
    if (window->pad_controller != NULL)
    {
        gtk_widget_remove_controller (GTK_WIDGET (window),
                                      GTK_EVENT_CONTROLLER (window->pad_controller));
        g_clear_weak_pointer (&window->pad_controller);
    }

    gtk_window_destroy (GTK_WINDOW (window));

#ifdef HAVE_MALLOC_TRIM
    schedule_trim ();
#endif
}

void
set_active_slot (NautilusWindow     *window,
                 NautilusWindowSlot *new_slot)
{
    NautilusWindowSlot *old_slot = window->active_slot;
    AdwTabView *view = new_slot != NULL ? get_tab_view_for_slot (window, new_slot) : NULL;

    g_return_if_fail (new_slot == NULL ||
                      gtk_widget_is_ancestor (GTK_WIDGET (new_slot), GTK_WIDGET (window)));

    if (view != NULL && window->split_enabled)
    {
        gboolean secondary = view == window->secondary_tab_view;
        if (window->secondary_is_active != secondary)
        {
            window->secondary_is_active = secondary;
            if (secondary)
            {
                gtk_widget_remove_css_class (window->main_pane, "active-pane");
                gtk_widget_add_css_class (window->secondary_pane, "active-pane");
            }
            else
            {
                gtk_widget_remove_css_class (window->secondary_pane, "active-pane");
                gtk_widget_add_css_class (window->main_pane, "active-pane");
            }
            update_shared_tab_widgets (window);
        }
    }

    if (old_slot == new_slot)
    {
        return;
    }

    g_debug ("Setting new slot %p as active, old slot inactive %p", new_slot, old_slot);

    /* make old slot inactive if it exists (may be NULL after init, for example) */
    if (old_slot != NULL)
    {
        /* inform slot & view */
        nautilus_window_slot_set_active (old_slot, FALSE);
    }

    g_set_weak_pointer (&window->active_slot, new_slot);

    /* make new slot active, if it exists */
    if (new_slot)
    {
        AdwTabPage *page = adw_tab_view_get_page (view, GTK_WIDGET (new_slot));
        adw_tab_view_set_selected_page (view, page);

        /* inform slot & view */
        nautilus_window_slot_set_active (new_slot, TRUE);

        update_cursor (window);
    }

    on_location_changed (window);

    g_object_notify_by_pspec (G_OBJECT (window), properties[PROP_ACTIVE_SLOT]);
}

static void
nautilus_window_realize (GtkWidget *widget)
{
    GTK_WIDGET_CLASS (nautilus_window_parent_class)->realize (widget);
    update_cursor (NAUTILUS_WINDOW (widget));
}

static gboolean
nautilus_window_key_bubble (GtkEventControllerKey *controller,
                            unsigned int           keyval,
                            unsigned int           keycode,
                            GdkModifierType        state,
                            gpointer               user_data)
{
    GtkWidget *widget;
    NautilusWindow *window;

    widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
    window = NAUTILUS_WINDOW (widget);
    if (window->active_slot != NULL &&
        nautilus_window_slot_handle_event (window->active_slot, controller, keyval, state))
    {
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

gboolean
nautilus_window_has_open_location (NautilusWindow *self,
                                   GFile          *location)
{
    g_return_val_if_fail (NAUTILUS_IS_WINDOW (self), FALSE);
    g_return_val_if_fail (location != NULL, FALSE);

    return get_slot_with_open_location (self, location) != NULL;
}

GFile *
nautilus_window_get_active_location (NautilusWindow *self)
{
    g_return_val_if_fail (NAUTILUS_IS_WINDOW (self), NULL);
    g_return_val_if_fail (self->active_slot != NULL, NULL);

    return nautilus_window_slot_get_location (self->active_slot);
}

GList *
nautilus_window_get_locations (NautilusWindow *self)
{
    g_return_val_if_fail (NAUTILUS_IS_WINDOW (self), NULL);

    GFileList *locations = NULL;

    for (GList *l = self->slots; l != NULL; l = l->next)
    {
        NautilusWindowSlot *slot = l->data;
        GFile *location = nautilus_window_slot_get_location (slot);

        if (location != NULL)
        {
            locations = g_list_prepend (locations, location);
        }
    }

    return locations;
}

static void
on_is_maximized_changed (GObject    *object,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
    gboolean is_maximized;

    is_maximized = gtk_window_is_maximized (GTK_WINDOW (object));

    g_settings_set_boolean (nautilus_window_state,
                            NAUTILUS_WINDOW_STATE_MAXIMIZED, is_maximized);
}

static gboolean
nautilus_window_close_request (GtkWindow *window)
{
    nautilus_window_close (NAUTILUS_WINDOW (window));

    /* Window got destroyed, don't call other GtkWindow::close-request handlers */
    return FALSE;
}

void
nautilus_window_back_or_forward_in_new_tab (NautilusWindow *window,
                                            int             distance)
{
    NautilusNavigationState *state;

    g_return_if_fail (NAUTILUS_IS_WINDOW (window));
    if (window->active_slot == NULL)
    {
        return;
    }

    state = nautilus_window_slot_get_navigation_state (window->active_slot);
    if (state == NULL)
    {
        return;
    }

    if ((distance == NAUTILUS_NAVIGATION_DIRECTION_FORWARD && state->forward_list == NULL) ||
        (distance == NAUTILUS_NAVIGATION_DIRECTION_BACK && state->back_list == NULL))
    {
        free_navigation_state (state);
        return;
    }

    /* Manually fix up the back / forward lists and location.
     * This way we don't have to unnecessary load the current location
     * and then load back / forward */
    if (distance == NAUTILUS_NAVIGATION_DIRECTION_FORWARD)
    {
        state->back_list = g_list_prepend (state->back_list, state->current_location_bookmark);
        state->current_location_bookmark = state->forward_list->data;
        state->forward_list = g_list_delete_link (state->forward_list, state->forward_list);
    }
    else if (distance == NAUTILUS_NAVIGATION_DIRECTION_BACK)
    {
        state->forward_list = g_list_prepend (state->forward_list, state->current_location_bookmark);
        state->current_location_bookmark = state->back_list->data;
        state->back_list = g_list_delete_link (state->back_list, state->back_list);
    }
    else
    {
        g_warn_if_reached ();
    }

    NautilusWindowSlot *new_slot = nautilus_window_create_and_init_slot (window);

    nautilus_window_slot_restore_navigation_state (new_slot, state);

    free_navigation_state (state);
}

static void
on_click_gesture_pressed (GtkGestureClick *gesture,
                          gint             n_press,
                          gdouble          x,
                          gdouble          y,
                          gpointer         user_data)
{
    GtkWidget *widget;
    NautilusWindow *window;
    guint button;

    widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    window = NAUTILUS_WINDOW (widget);
    button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

    if (window->active_slot == NULL)
    {
        return;
    }

    if (nautilus_global_preferences_get_use_extra_buttons () &&
        (button == nautilus_global_preferences_get_back_button ()))
    {
        nautilus_window_slot_navigate (window->active_slot, -1);
    }
    else if (nautilus_global_preferences_get_use_extra_buttons () &&
             (button == nautilus_global_preferences_get_forward_button ()))
    {
        nautilus_window_slot_navigate (window->active_slot, 1);
    }
}

static void
nautilus_window_init (NautilusWindow *window)
{
    GtkWindowGroup *window_group;
    GtkEventController *controller;
    GtkPadController *pad_controller;

    g_type_ensure (NAUTILUS_TYPE_NETWORK_ADDRESS_BAR);
    g_type_ensure (NAUTILUS_TYPE_TOOLBAR);
    g_type_ensure (NAUTILUS_TYPE_PLACES_SIDEBAR);
    g_type_ensure (NAUTILUS_TYPE_PROGRESS_INDICATOR);
    g_type_ensure (NAUTILUS_TYPE_SHORTCUT_MANAGER);
    gtk_widget_init_template (GTK_WIDGET (window));

    adw_window_title_set_title (window->window_title, APPLICATION_NAME);

    window->split_enabled = FALSE;
    window->secondary_is_active = FALSE;
    window->closing_page = FALSE;
    window->closing = FALSE;
    window->collapse_source_id = 0;
    gtk_widget_set_visible (window->secondary_pane, FALSE);
    gtk_widget_set_visible (GTK_WIDGET (window->secondary_tab_bar), FALSE);
    gtk_widget_set_visible (window->secondary_tab_bar_container, FALSE);
    gtk_widget_add_css_class (window->main_pane, "active-pane");

    g_object_bind_property (window->paned, "position",
                            window->tab_bars_paned, "position",
                            G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

    g_signal_connect (window->paned, "notify::position",
                      G_CALLBACK (pane_position_changed_cb), window);

    controller = gtk_event_controller_focus_new ();
    g_signal_connect (controller, "enter", G_CALLBACK (pane_focus_entered_cb), window);
    gtk_widget_add_controller (window->main_pane, controller);

    controller = gtk_event_controller_focus_new ();
    g_signal_connect (controller, "enter", G_CALLBACK (pane_focus_entered_cb), window);
    gtk_widget_add_controller (window->secondary_pane, controller);

    controller = gtk_event_controller_focus_new ();
    g_signal_connect (controller, "enter", G_CALLBACK (pane_focus_entered_cb), window);
    gtk_widget_add_controller (GTK_WIDGET (window->main_tab_bar), controller);

    controller = gtk_event_controller_focus_new ();
    g_signal_connect (controller, "enter", G_CALLBACK (pane_focus_entered_cb), window);
    gtk_widget_add_controller (GTK_WIDGET (window->secondary_tab_bar), controller);

    g_signal_connect_object (nautilus_preferences, "changed::vinder-blur",
                             G_CALLBACK (update_files_header_blur), window, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_preferences, "changed::vinder-scroll-animation",
                             G_CALLBACK (update_files_header_blur), window, G_CONNECT_SWAPPED);
    g_signal_connect_object (adw_style_manager_get_default (), "notify::high-contrast",
                             G_CALLBACK (update_files_header_blur), window, G_CONNECT_SWAPPED);
    update_files_header_blur (window);

    g_signal_connect_object (window->files_toolbar_view, "notify::top-bar-height",
                             G_CALLBACK (report_top_bar_height), window, G_CONNECT_SWAPPED);
    report_top_bar_height (window);

    g_signal_connect_object (nautilus_preferences, "changed::vinder-pop-icons",
                             G_CALLBACK (update_file_icon_shadows), window, G_CONNECT_SWAPPED);
    update_file_icon_shadows (window);

    g_signal_connect_object (nautilus_preferences, "changed::vinder-zebra-rows",
                             G_CALLBACK (update_zebra_rows), window, G_CONNECT_SWAPPED);
    update_zebra_rows (window);

    g_signal_connect (window, "notify::maximized",
                      G_CALLBACK (on_is_maximized_changed), NULL);

    window->slots = NULL;
    window->active_slot = NULL;

    gtk_widget_add_css_class (GTK_WIDGET (window), "nautilus-window");

    window_group = gtk_window_group_new ();
    gtk_window_group_add_window (window_group, GTK_WINDOW (window));
    g_object_unref (window_group);

    window->tab_data_queue = g_queue_new ();

    /* Attention: this creates a reference cycle: the pad controller owns a
     * reference to the window (as an action group) and the window (as a widget)
     * owns a reference to the pad controller. To break this, we must remove
     * the controller from the window before destroying the window. But we need
     * to know the controller is still alive before trying to remove it, so a
     * weak reference is added. */
    pad_controller = gtk_pad_controller_new (G_ACTION_GROUP (window), NULL);
    g_set_weak_pointer (&window->pad_controller, pad_controller);
    gtk_pad_controller_set_action_entries (window->pad_controller,
                                           pad_actions, G_N_ELEMENTS (pad_actions));
    gtk_widget_add_controller (GTK_WIDGET (window),
                               GTK_EVENT_CONTROLLER (window->pad_controller));

    controller = GTK_EVENT_CONTROLLER (gtk_gesture_click_new ());
    gtk_widget_add_controller (GTK_WIDGET (window), controller);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (controller), 0);
    g_signal_connect (controller, "pressed",
                      G_CALLBACK (on_click_gesture_pressed), NULL);

    controller = gtk_event_controller_key_new ();
    gtk_widget_add_controller (GTK_WIDGET (window), controller);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_BUBBLE);
    g_signal_connect (controller, "key-pressed",
                      G_CALLBACK (nautilus_window_key_bubble), NULL);
}

static void
nautilus_window_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    NautilusWindow *self = NAUTILUS_WINDOW (object);

    switch (prop_id)
    {
        case PROP_ACTIVE_SLOT:
        {
            g_value_set_object (value, G_OBJECT (self->active_slot));
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_window_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    NautilusWindow *self = NAUTILUS_WINDOW (object);

    switch (prop_id)
    {
        case PROP_ACTIVE_SLOT:
        {
            set_active_slot (self, NAUTILUS_WINDOW_SLOT (g_value_get_object (value)));
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_window_class_init (NautilusWindowClass *class)
{
    GObjectClass *oclass = G_OBJECT_CLASS (class);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS (class);
    GtkWindowClass *winclass = GTK_WINDOW_CLASS (class);

    oclass->dispose = nautilus_window_dispose;
    oclass->finalize = nautilus_window_finalize;
    oclass->constructed = nautilus_window_constructed;
    oclass->get_property = nautilus_window_get_property;
    oclass->set_property = nautilus_window_set_property;

    wclass->realize = nautilus_window_realize;
    wclass->grab_focus = nautilus_window_grab_focus;

    winclass->close_request = nautilus_window_close_request;

    properties[PROP_ACTIVE_SLOT] =
        g_param_spec_object ("active-slot",
                             NULL, NULL,
                             NAUTILUS_TYPE_WINDOW_SLOT,
                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (oclass, N_PROPS, properties);

    gtk_widget_class_set_template_from_resource (wclass,
                                                 "/org/gnome/nautilus/ui/nautilus-window.ui");
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, undo_redo_section);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, toolbar);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, window_title);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, split_view);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, files_toolbar_view);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, places_sidebar);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, toast_overlay);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, tab_overview);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, tab_view);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, secondary_tab_view);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, main_tab_bar);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, secondary_tab_bar);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, secondary_tab_bar_container);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, main_tab_strip);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, secondary_tab_strip);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, tab_bars_paned);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, tab_button);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, secondary_pane);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, main_pane);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, paned);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, network_address_bar);

    gtk_widget_class_bind_template_callback (wclass, create_tab_cb);

    gtk_widget_class_bind_template_callback (wclass, tab_view_close_page_cb);
    gtk_widget_class_bind_template_callback (wclass, tab_view_setup_menu_cb);
    gtk_widget_class_bind_template_callback (wclass, tab_view_notify_selected_page_cb);
    gtk_widget_class_bind_template_callback (wclass, tab_view_create_window_cb);
    gtk_widget_class_bind_template_callback (wclass, tab_view_page_attached_cb);
    gtk_widget_class_bind_template_callback (wclass, tab_view_page_detached_cb);
    gtk_widget_class_bind_template_callback (wclass, tab_view_page_reordered_cb);

    signals[LOCATIONS_CHANGED] =
        g_signal_new ("locations-changed",
                      G_TYPE_FROM_CLASS (class),
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}

NautilusWindow *
nautilus_window_new (void)
{
    return g_object_new (NAUTILUS_TYPE_WINDOW,
                         "icon-name", APPLICATION_ID,
                         NULL);
}

void
nautilus_window_show_about_dialog (NautilusWindow *window)
{
    AdwDialog *dialog;
    g_autofree gchar *module_names = nautilus_module_get_installed_module_names ();
    g_autofree gchar *debug_info = NULL;

    const gchar *vinder_developers[] =
    {
        "Paul Beyens (paullinux-gn)",
        NULL
    };
    const gchar *upstream_authors[] =
    {
        "The GNOME Project https://gitlab.gnome.org/GNOME/nautilus",
        "Sun Microsystems",
        NULL
    };

    if (module_names == NULL)
    {
        debug_info = g_strdup (_("No plugins currently installed."));
    }
    else
    {
        debug_info = g_strconcat (_("Currently installed plugins:"), "\n\n",
                                  module_names, "\n\n",
                                  _("For bug testing only, the following command can be used:"), "\n"
                                  "NAUTILUS_DISABLE_PLUGINS=TRUE nautilus", NULL);
    }

    dialog = adw_about_dialog_new_from_appdata ("/org/gnome/nautilus/appdata", NULL);

    adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), APPLICATION_NAME);
    adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (dialog), APPLICATION_ID);
    adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "Paul Beyens");
    adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (dialog),
                                   _("A GNOME file manager based on Nautilus."));
    adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog), "https://github.com/paullinux-gn/vinder");
    adw_about_dialog_set_issue_url (ADW_ABOUT_DIALOG (dialog), "https://github.com/paullinux-gn/vinder/issues");
    adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), VERSION);
    adw_about_dialog_set_debug_info (ADW_ABOUT_DIALOG (dialog), debug_info);
    adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog),
                                     "© 2026 Paul Beyens (Vinder modifications)\n"
                                     "© 1999 The Files Authors (upstream Nautilus)");
    adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (dialog), vinder_developers);
    adw_about_dialog_add_acknowledgement_section (ADW_ABOUT_DIALOG (dialog),
                                                  _("Upstream Nautilus"), upstream_authors);
    adw_about_dialog_set_support_url (ADW_ABOUT_DIALOG (dialog), "https://github.com/paullinux-gn/vinder/issues");
    const gchar *upstream_translators = _("translator-credits");
    if (g_strcmp0 (upstream_translators, "translator-credits") != 0)
    {
        const gchar *translation_credits[] = { upstream_translators, NULL };
        adw_about_dialog_add_acknowledgement_section (ADW_ABOUT_DIALOG (dialog),
                                                      _("Upstream Nautilus translations"),
                                                      translation_credits);
    }

    adw_dialog_present (dialog, window ? GTK_WIDGET (window) : NULL);
}

void
nautilus_window_search (NautilusWindow *window,
                        NautilusQuery  *query)
{
    if (window->active_slot != NULL)
    {
        nautilus_window_slot_search (window->active_slot, query);
    }
    else
    {
        g_warning ("Trying search on a slot but no active slot present");
    }
}
