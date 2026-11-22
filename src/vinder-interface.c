/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <config.h>

#include "vinder-interface.h"

#include <glib/gi18n.h>

#include "nautilus-global-preferences.h"
#include "vinder-blur-css.h"

static void
update_appearance (GSettings           *settings,
                   const char          *key,
                   NautilusApplication *self)
{
    g_autofree char *appearance = g_settings_get_string (settings, key);
    GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), key);
    gboolean mixed = g_str_equal (appearance, "dark-and-light");

    g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_string (appearance));
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                       g_str_equal (appearance, "light")
                                       ? ADW_COLOR_SCHEME_FORCE_LIGHT
                                       : ADW_COLOR_SCHEME_FORCE_DARK);

    for (GList *l = nautilus_application_get_windows (self); l != NULL; l = l->next)
    {
        GtkWidget *window = GTK_WIDGET (l->data);

        if (mixed)
        {
            gtk_widget_add_css_class (window, "vinder-dark-and-light");
        }
        else
        {
            gtk_widget_remove_css_class (window, "vinder-dark-and-light");
        }
    }
}

static void
change_appearance (GSimpleAction *action,
                   GVariant      *value,
                   gpointer       user_data)
{
    g_settings_set_value (nautilus_preferences, g_action_get_name (G_ACTION (action)), value);
}

/* Per-application state of the blur stylesheet. */
typedef struct
{
    int top_bar_height;             /* real sizes reported by the windows */
    int column_header_height;
    char *css;                      /* what the provider currently holds */
    guint update_source;            /* pending refresh of the stylesheet */
} InterfaceState;

static void
interface_state_free (InterfaceState *state)
{
    g_clear_handle_id (&state->update_source, g_source_remove);
    g_free (state->css);
    g_free (state);
}

static InterfaceState *
interface_state (NautilusApplication *self)
{
    return g_object_get_data (G_OBJECT (self), "vinder-interface-state");
}

static void
update_blur_css (NautilusApplication *self)
{
    InterfaceState *state = interface_state (self);
    g_autofree char *css = NULL;

    if (g_settings_get_boolean (nautilus_preferences, "vinder-blur"))
    {
        css = vinder_blur_css_build (g_settings_get_double (nautilus_preferences, "vinder-blur-opacity"),
                                     g_settings_get_double (nautilus_preferences, "vinder-blur-strength"),
                                     g_settings_get_boolean (nautilus_preferences, "vinder-menu-blur"),
                                     state->top_bar_height,
                                     state->column_header_height);
    }

    /* Loading a stylesheet restyles every widget, so skip it when nothing changed. */
    if (g_strcmp0 (css != NULL ? css : "", state->css) != 0)
    {
        GtkCssProvider *provider = g_object_get_data (G_OBJECT (self), "blur-provider");

        g_free (state->css);
        state->css = g_strdup (css != NULL ? css : "");
        gtk_css_provider_load_from_string (provider, state->css);
    }
}

static gboolean
run_blur_update (gpointer user_data)
{
    NautilusApplication *self = user_data;

    interface_state (self)->update_source = 0;
    update_blur_css (self);

    return G_SOURCE_REMOVE;
}

/* Several settings (or sizes) often change together; refresh the stylesheet once for all. */
static void
schedule_blur_update (NautilusApplication *self)
{
    InterfaceState *state = interface_state (self);

    if (state->update_source == 0)
    {
        state->update_source = g_idle_add (run_blur_update, self);
    }
}

static void
sync_blur_toggle (NautilusApplication *self)
{
    GAction *toggle = g_action_map_lookup_action (G_ACTION_MAP (self), "vinder-blur");

    g_simple_action_set_state (G_SIMPLE_ACTION (toggle),
                              g_variant_new_boolean (g_settings_get_boolean (nautilus_preferences,
                                                                              "vinder-blur")));
}

static void
on_blur_setting_changed (GSettings           *settings,
                         const char          *key,
                         NautilusApplication *self)
{
    sync_blur_toggle (self);
    schedule_blur_update (self);
}

void
vinder_interface_set_top_bar_height (NautilusApplication *self,
                                     int                  height)
{
    InterfaceState *state = interface_state (self);

    if (state != NULL && height > 0 && height != state->top_bar_height)
    {
        state->top_bar_height = height;
        schedule_blur_update (self);
    }
}

void
vinder_interface_set_column_header_height (NautilusApplication *self,
                                           int                  height)
{
    InterfaceState *state = interface_state (self);

    if (state != NULL && height > 0 && height != state->column_header_height)
    {
        state->column_header_height = height;
        schedule_blur_update (self);
    }
}

static void
change_blur (GSimpleAction *action,
             GVariant      *value,
             gpointer       user_data)
{
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (user_data));

    if (!g_settings_set_value (nautilus_preferences, "vinder-blur", value) ||
        !g_variant_get_boolean (value) || window == NULL ||
        g_settings_get_boolean (nautilus_preferences, "vinder-blur-reminder-seen"))
    {
        return;
    }

    AdwDialog *dialog = adw_alert_dialog_new (_("Sidebar Blur"),
                                              _("Desktop blur on GNOME requires GNOME 51 or later."));

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "ok", _("_OK"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "ok");
    adw_dialog_present (dialog, GTK_WIDGET (window));
    g_settings_set_boolean (nautilus_preferences, "vinder-blur-reminder-seen", TRUE);
}

static void
update_icon_shadow_strength (GSettings           *settings,
                             const char          *key,
                             NautilusApplication *self)
{
    int strength = (int) g_settings_get_double (settings, "vinder-icon-shadow-strength");
    g_autofree char *css = g_strdup_printf (
        ".vinder-content.pop-icons { --vinder-icon-shadow-strength: %d%%;"
        " --vinder-icon-shadow-lift: %d; }", strength, strength);

    GtkCssProvider *provider = g_object_get_data (G_OBJECT (self), "icon-shadow-provider");

    gtk_css_provider_load_from_string (provider, css);
}

/* Dragging a slider fires a change for every step. Writing each one to the settings
 * would restyle every window each time, so the value is only written once the slider
 * has been still for a moment. */
#define SLIDER_COMMIT_DELAY_MS 100

typedef struct
{
    GtkAdjustment *adjustment;      /* not owned: this record lives as long as it does */
    char *key;
    double pending;
    guint timeout_id;
    gulong adjustment_handler;
    gulong settings_handler;
} SliderBinding;

static void
slider_binding_commit (SliderBinding *binding)
{
    g_settings_set_double (nautilus_preferences, binding->key, round (binding->pending));
}

static gboolean
on_slider_timeout (gpointer user_data)
{
    SliderBinding *binding = user_data;

    binding->timeout_id = 0;
    slider_binding_commit (binding);

    return G_SOURCE_REMOVE;
}

static void
on_slider_value_changed (GtkAdjustment *adjustment,
                         SliderBinding *binding)
{
    binding->pending = gtk_adjustment_get_value (adjustment);
    g_clear_handle_id (&binding->timeout_id, g_source_remove);
    binding->timeout_id = g_timeout_add (SLIDER_COMMIT_DELAY_MS, on_slider_timeout, binding);
}

static void
on_slider_setting_changed (GSettings     *settings,
                           const char    *key,
                           SliderBinding *binding)
{
    double value;

    if (binding->timeout_id != 0)
    {
        return;     /* the user is still dragging; their value wins */
    }

    value = g_settings_get_double (settings, binding->key);
    if (value != gtk_adjustment_get_value (binding->adjustment))
    {
        g_signal_handler_block (binding->adjustment, binding->adjustment_handler);
        gtk_adjustment_set_value (binding->adjustment, value);
        g_signal_handler_unblock (binding->adjustment, binding->adjustment_handler);
    }
}

static void
slider_binding_free (SliderBinding *binding)
{
    g_signal_handler_disconnect (nautilus_preferences, binding->settings_handler);

    if (binding->timeout_id != 0)
    {
        /* The dialog was closed right after a change: do not lose it. */
        g_source_remove (binding->timeout_id);
        slider_binding_commit (binding);
    }

    g_free (binding->key);
    g_free (binding);
}

static GtkWidget *
add_interface_slider (AdwPreferencesGroup *group,
                      const char          *title,
                      const char          *key,
                      int                  maximum)
{
    AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
    GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, maximum, 1);
    GtkAdjustment *adjustment = gtk_range_get_adjustment (GTK_RANGE (scale));
    g_autofree char *changed_signal = g_strconcat ("changed::", key, NULL);
    SliderBinding *binding = g_new0 (SliderBinding, 1);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
    gtk_scale_set_digits (GTK_SCALE (scale), 0);
    gtk_widget_set_size_request (scale, 150, -1);
    gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
    gtk_accessible_update_property (GTK_ACCESSIBLE (scale),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);

    binding->adjustment = adjustment;
    binding->key = g_strdup (key);
    gtk_adjustment_set_value (adjustment, g_settings_get_double (nautilus_preferences, key));
    binding->adjustment_handler = g_signal_connect (adjustment, "value-changed",
                                                    G_CALLBACK (on_slider_value_changed), binding);
    binding->settings_handler = g_signal_connect (nautilus_preferences, changed_signal,
                                                  G_CALLBACK (on_slider_setting_changed), binding);
    g_object_set_data_full (G_OBJECT (adjustment), "vinder-slider-binding", binding,
                            (GDestroyNotify) slider_binding_free);

    adw_action_row_add_suffix (row, scale);
    adw_preferences_group_add (group, GTK_WIDGET (row));

    return GTK_WIDGET (row);
}

static void
action_interface (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (user_data));
    AdwDialog *dialog = adw_preferences_dialog_new ();
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

    AdwPreferencesGroup *themes = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    const char *theme_labels[] = { _("Light"), _("Dark"), _("Dark and Light") };
    const char *theme_values[] = { "light", "dark", "dark-and-light" };
    GtkCheckButton *first = NULL;
    AdwSwitchRow *blur = ADW_SWITCH_ROW (adw_switch_row_new ());
    AdwSwitchRow *menu_blur = ADW_SWITCH_ROW (adw_switch_row_new ());
    AdwSwitchRow *scroll_animation = ADW_SWITCH_ROW (adw_switch_row_new ());
    AdwSwitchRow *pop_icons = ADW_SWITCH_ROW (adw_switch_row_new ());

    adw_dialog_set_title (dialog, _("Interface"));
    adw_dialog_set_content_width (dialog, 360);
    adw_preferences_group_set_title (themes, _("Appearance"));
    for (guint i = 0; i < G_N_ELEMENTS (theme_labels); i++)
    {
        GtkCheckButton *choice = GTK_CHECK_BUTTON (gtk_check_button_new_with_label (theme_labels[i]));

        if (first == NULL)
        {
            first = choice;
        }
        else
        {
            gtk_check_button_set_group (choice, first);
        }
        gtk_actionable_set_action_target (GTK_ACTIONABLE (choice), "s", theme_values[i]);
        gtk_actionable_set_action_name (GTK_ACTIONABLE (choice), "app.vinder-appearance");
        adw_preferences_group_add (themes, GTK_WIDGET (choice));
    }
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pop_icons), _("Pop the Icons"));
    g_settings_bind (nautilus_preferences, "vinder-pop-icons", pop_icons, "active",
                     G_SETTINGS_BIND_DEFAULT);
    adw_preferences_group_add (themes, GTK_WIDGET (pop_icons));
    GtkWidget *shadow_strength = add_interface_slider (themes, _("Shadow Strength (%)"),
                                                       "vinder-icon-shadow-strength", 100);
    g_settings_bind (nautilus_preferences, "vinder-pop-icons", shadow_strength, "visible",
                     G_SETTINGS_BIND_GET);
    AdwSwitchRow *zebra_rows = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (zebra_rows), _("Alternating List Row Colors"));
    g_settings_bind (nautilus_preferences, "vinder-zebra-rows", zebra_rows, "active",
                     G_SETTINGS_BIND_DEFAULT);
    adw_preferences_group_add (themes, GTK_WIDGET (zebra_rows));
    adw_preferences_page_add (page, themes);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (blur), _("Blur"));
    gtk_actionable_set_action_name (GTK_ACTIONABLE (blur), "app.vinder-blur");
    adw_preferences_group_add (group, GTK_WIDGET (blur));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (menu_blur), _("Blur Menus"));
    g_settings_bind (nautilus_preferences, "vinder-menu-blur", menu_blur, "active",
                     G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_NO_SENSITIVITY);
    g_settings_bind (nautilus_preferences, "vinder-blur", menu_blur, "sensitive", G_SETTINGS_BIND_GET);
    adw_preferences_group_add (group, GTK_WIDGET (menu_blur));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (scroll_animation), _("Scroll Animation"));
    g_settings_bind (nautilus_preferences, "vinder-scroll-animation", scroll_animation, "active",
                     G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_NO_SENSITIVITY);
    g_settings_bind (nautilus_preferences, "vinder-blur", scroll_animation, "sensitive", G_SETTINGS_BIND_GET);
    adw_preferences_group_add (group, GTK_WIDGET (scroll_animation));
    adw_preferences_page_add (page, group);

    AdwPreferencesGroup *sliders = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    g_autofree char *header_limits = g_strdup_printf (
        _("With Scroll Animation on, toolbars and column headers use at most %d%% opacity and %d blur, so content stays readable behind them."),
        VINDER_HEADER_MAX_OPACITY, VINDER_HEADER_MAX_BLUR);

    adw_preferences_group_set_description (sliders, header_limits);
    add_interface_slider (sliders, _("Opacity (%)"), "vinder-blur-opacity", 100);
    add_interface_slider (sliders, _("Blur Strength"), "vinder-blur-strength", 40);
    g_settings_bind (nautilus_preferences, "vinder-blur", sliders, "sensitive", G_SETTINGS_BIND_GET);
    adw_preferences_page_add (page, sliders);
    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), page);
    adw_dialog_present (dialog, GTK_WIDGET (window));
}

static const GActionEntry interface_entries[] =
{
    { .name = "vinder-appearance", .parameter_type = "s", .state = "\"light\"", .change_state = change_appearance },
    { .name = "vinder-blur", .state = "false", .change_state = change_blur },
    { .name = "vinder-interface", .activate = action_interface },
};

void
vinder_interface_setup (NautilusApplication *self)
{
    g_action_map_add_action_entries (G_ACTION_MAP (self),
                                     interface_entries, G_N_ELEMENTS (interface_entries),
                                     self);

    g_signal_connect_object (nautilus_preferences, "changed::vinder-appearance",
                             G_CALLBACK (update_appearance), self, 0);
    update_appearance (nautilus_preferences, "vinder-appearance", self);

    InterfaceState *state = g_new0 (InterfaceState, 1);

    state->top_bar_height = VINDER_DEFAULT_TOP_BAR_HEIGHT;
    state->column_header_height = VINDER_DEFAULT_COLUMN_HEADER_HEIGHT;
    g_object_set_data_full (G_OBJECT (self), "vinder-interface-state", state,
                            (GDestroyNotify) interface_state_free);

    GtkCssProvider *blur_provider = gtk_css_provider_new ();
    g_object_set_data_full (G_OBJECT (self), "blur-provider", blur_provider, g_object_unref);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (blur_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    const char *blur_signals[] = {
        "changed::vinder-blur",
        "changed::vinder-menu-blur",
        "changed::vinder-blur-opacity",
        "changed::vinder-blur-strength",
    };
    for (guint i = 0; i < G_N_ELEMENTS (blur_signals); i++)
    {
        g_signal_connect_object (nautilus_preferences, blur_signals[i],
                                 G_CALLBACK (on_blur_setting_changed), self, 0);
    }
    sync_blur_toggle (self);
    update_blur_css (self);

    GtkCssProvider *icon_shadow_provider = gtk_css_provider_new ();
    g_object_set_data_full (G_OBJECT (self), "icon-shadow-provider", icon_shadow_provider, g_object_unref);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (icon_shadow_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_signal_connect_object (nautilus_preferences, "changed::vinder-icon-shadow-strength",
                             G_CALLBACK (update_icon_shadow_strength), self, 0);
    update_icon_shadow_strength (nautilus_preferences, NULL, self);
}

void
vinder_interface_apply_window_appearance (GtkWindow *window)
{
    g_autofree char *appearance = g_settings_get_string (nautilus_preferences, "vinder-appearance");

    if (g_str_equal (appearance, "dark-and-light"))
    {
        gtk_widget_add_css_class (GTK_WIDGET (window), "vinder-dark-and-light");
    }
}
