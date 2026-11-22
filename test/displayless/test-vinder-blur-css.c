/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <glib.h>
#include <string.h>

#include "vinder-blur-css.h"

static void
assert_contains (const char *css,
                 const char *expected)
{
    if (strstr (css, expected) == NULL)
    {
        g_test_fail_printf ("Missing in the stylesheet: %s", expected);
    }
}

static void
assert_lacks (const char *css,
              const char *unexpected)
{
    if (strstr (css, unexpected) != NULL)
    {
        g_test_fail_printf ("Unexpectedly in the stylesheet: %s", unexpected);
    }
}

static void
test_default_sizes_keep_the_old_layout (void)
{
    g_autofree char *css = vinder_blur_css_build (78, 30, TRUE,
                                                  VINDER_DEFAULT_TOP_BAR_HEIGHT,
                                                  VINDER_DEFAULT_COLUMN_HEADER_HEIGHT);

    assert_contains (css, "gridview { padding-top: 66px; }");
    assert_contains (css, "margin-top: -112px;");
    assert_contains (css, "padding-top: calc(112px + 16px);");
    assert_contains (css, "min-height: 84px;");
    assert_contains (css, "transparent 40px,");
    assert_contains (css, "var(--vinder-header-tint) 48px,");
    assert_contains (css, "55%, transparent) 60px,");
}

static void
test_room_follows_the_real_sizes (void)
{
    /* Large text: taller toolbar and column header. */
    g_autofree char *css = vinder_blur_css_build (78, 30, TRUE, 90, 50);

    assert_contains (css, "gridview { padding-top: 90px; }");
    assert_contains (css, "margin-top: -140px;");
    assert_contains (css, "padding-top: calc(140px + 16px);");
    assert_contains (css, "min-height: 108px;");
    assert_contains (css, "transparent 64px,");
    assert_contains (css, "var(--vinder-header-tint) 72px,");
    assert_contains (css, "55%, transparent) 84px,");

    /* None of the old fixed numbers are left behind. */
    assert_lacks (css, "padding-top: 66px");
    assert_lacks (css, "-112px");
}

static void
test_nonsense_sizes_do_not_break_the_gradient (void)
{
    g_autofree char *css = vinder_blur_css_build (78, 30, TRUE, 0, -5);

    /* A toolbar cannot be smaller than 32px, so no stop goes negative. */
    assert_contains (css, "transparent 6px,");
    assert_contains (css, "var(--vinder-header-tint) 14px,");
    assert_contains (css, "margin-top: -32px;");
    assert_lacks (css, "transparent -");
    assert_lacks (css, "header-tint) -");
}

static void
test_header_is_capped_but_other_surfaces_are_not (void)
{
    g_autofree char *css = vinder_blur_css_build (100, 40, TRUE,
                                                  VINDER_DEFAULT_TOP_BAR_HEIGHT,
                                                  VINDER_DEFAULT_COLUMN_HEADER_HEIGHT);

    /* The toolbar and column headers stop at the documented limits... */
    assert_contains (css, "var(--headerbar-bg-color) 62%");
    assert_contains (css, "blur(18px);");
}

static void
test_header_cap_values_match_the_documented_ones (void)
{
    g_autofree char *css = vinder_blur_css_build (100, 40, FALSE,
                                                  VINDER_DEFAULT_TOP_BAR_HEIGHT,
                                                  VINDER_DEFAULT_COLUMN_HEADER_HEIGHT);
    g_autofree char *capped_opacity = g_strdup_printf ("var(--headerbar-bg-color) %d%%", VINDER_HEADER_MAX_OPACITY);
    g_autofree char *capped_blur = g_strdup_printf ("blur(%dpx);", VINDER_HEADER_MAX_BLUR);

    assert_contains (css, capped_opacity);
    assert_contains (css, capped_blur);

    /* ...while the sidebar and popups use the full slider values. */
    assert_contains (css, "var(--sidebar-bg-color) 100%");
    assert_contains (css, "blur(40px);");
}

static void
test_below_the_cap_the_sliders_apply_as_they_are (void)
{
    g_autofree char *css = vinder_blur_css_build (40, 10, FALSE,
                                                  VINDER_DEFAULT_TOP_BAR_HEIGHT,
                                                  VINDER_DEFAULT_COLUMN_HEADER_HEIGHT);

    assert_contains (css, "var(--headerbar-bg-color) 40%");
    assert_contains (css, "blur(10px);");
    assert_lacks (css, "62%");
}

static void
test_menu_blur_can_be_switched_off (void)
{
    g_autofree char *with_menus = vinder_blur_css_build (78, 30, TRUE, 66, 46);
    g_autofree char *without_menus = vinder_blur_css_build (78, 30, FALSE, 66, 46);

    assert_contains (with_menus, "popover > contents");
    assert_lacks (without_menus, "popover > contents");
}

int
main (int   argc,
      char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/vinder-blur-css/default-sizes-keep-the-old-layout",
                     test_default_sizes_keep_the_old_layout);
    g_test_add_func ("/vinder-blur-css/room-follows-the-real-sizes",
                     test_room_follows_the_real_sizes);
    g_test_add_func ("/vinder-blur-css/nonsense-sizes",
                     test_nonsense_sizes_do_not_break_the_gradient);
    g_test_add_func ("/vinder-blur-css/header-capped-others-not",
                     test_header_is_capped_but_other_surfaces_are_not);
    g_test_add_func ("/vinder-blur-css/cap-values-match-documented",
                     test_header_cap_values_match_the_documented_ones);
    g_test_add_func ("/vinder-blur-css/below-the-cap",
                     test_below_the_cap_the_sliders_apply_as_they_are);
    g_test_add_func ("/vinder-blur-css/menu-blur-switch",
                     test_menu_blur_can_be_switched_off);

    return g_test_run ();
}
