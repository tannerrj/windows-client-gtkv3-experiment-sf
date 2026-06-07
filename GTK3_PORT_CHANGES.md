# GTK3 Port Changes

This document describes every source-level change made to port the Crossfire
GTK client from GTK2 to GTK3, the rationale behind each decision, and any
design constraints that shaped the implementation.  It is intended for
developers who need to understand, maintain, or extend this codebase.

---

## Background

The GTK client (`gtk-v2/`) was written against GTK2 and accumulated a large
number of APIs that were deprecated across GTK 3.0-3.22 and finally removed in
GTK 4.  The port targets GTK 3.24 (the current LTS release and the last 3.x
series), which still ships all the removed GTK2 APIs as deprecated stubs.
Compiling with `-Wno-deprecated-declarations` lets the tree build cleanly while
we iteratively replace each deprecated call.

The port was validated with two build configurations:

| Build | CMake flags | Purpose |
|-------|-------------|---------|
| Normal | _(default)_ | Ensures daily build is not broken |
| Strict | `-DGDK_DISABLE_DEPRECATED -DGTK_DISABLE_DEPRECATED` | Catches APIs that will not compile against GTK 4 |

All changes described below pass both configurations.

---

## Build System (`CMakeLists.txt`)

### GTK package target

```diff
-pkg_check_modules(GTK gtk+-2.0 gio-2.0 REQUIRED)
+pkg_check_modules(GTK gtk+-3.0 gio-2.0 REQUIRED)
```

The package name for the GTK3 pkg-config module is `gtk+-3.0`.  GIO is kept as
a separate dependency because not all GTK3 installations bundle a
GIO-with-networking header (`gio/gnetworking.h`), which is tested separately
with `check_include_files`.

### Capsicum sandbox

Capsicum (FreeBSD capability sandbox) support is gated on `HAVE_CAPSICUM`,
which CMake detects by probing for `sys/capsicum.h`.  On Linux, macOS, and
Windows the header is absent so `HAVE_CAPSICUM` is not defined, the sandbox
checkbox in the metaserver dialog is shown but greyed out, and no Capsicum
code is compiled.  On FreeBSD the header is present, the checkbox is enabled,
and `cap_enter()` is called after the server connection is established.

```cmake
check_include_files(sys/capsicum.h HAVE_CAPSICUM)
```

`config.h.in` exposes the result to C code:

```c
#cmakedefine HAVE_CAPSICUM
```

**`gtk-v2/ui/dialogs.ui`** - An "Enable Sandbox" `GtkCheckButton` with
`id="sandbox_enable"` was added to the metaserver dialog's button row, before
the Connect button.  The widget's `sensitive` property defaults to `False` in
the UI file; `init_ui()` in `main.c` overrides this based on `HAVE_CAPSICUM`.

**`gtk-v2/src/main.c`** - Three changes:

1. Conditional include at the top of the file:

```c
#ifdef HAVE_CAPSICUM
#include <sys/capsicum.h>
#endif
```

2. `init_ui()` looks up the widget and enables it only on FreeBSD:

```c
sandbox_enable = GTK_CHECK_BUTTON(gtk_builder_get_object(dialog_xml, "sandbox_enable"));
#ifdef HAVE_CAPSICUM
gtk_widget_set_sensitive(GTK_WIDGET(sandbox_enable), true);
#else
gtk_widget_set_sensitive(GTK_WIDGET(sandbox_enable), false);
#endif
```

3. In the main connection loop, before `client_negotiate()`, the sandbox is
entered if the user checked the box.  Theme assets and the Cairo label font
are pre-loaded first because `cap_enter()` denies all further filesystem
access:

```c
map_pre_sandbox_init();
sandbox_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sandbox_enable));
if (sandbox_enabled) {
    gtk_widget_show(window_root);
    map_init(window_root);
    for (int i = 0; i < 100; i++) {
        gtk_main_iteration();
    }
    gtk_widget_hide(window_root);

#ifdef HAVE_CAPSICUM
    if (cap_enter() != 0) {
        error_dialog("Failed to enter sandbox",
                     "Sandboxing was enabled, but the running kernel does not support sandboxing.");
        break;
    }
    LOG(LOG_INFO, "main", "Entering sandbox");
#endif
}
```

After the event loop returns, the loop also breaks if sandboxing was active -
reconnecting would require filesystem access that the sandbox no longer permits:

```c
if (sandbox_enabled) {
    break;
}
```

**`gtk-v2/src/map.c`** - `map_draw_labels()` previously created and destroyed a
`cairo_font_face_t` on every draw call.  Under Capsicum, the first draw call
after `cap_enter()` would fail to load the font because font file lookups are
denied.  The fix promotes the font to a module-level static and provides a
`map_pre_sandbox_init()` function that creates and warms up the font before the
sandbox is entered:

```c
// Module-level:
static cairo_font_face_t *font;

void map_pre_sandbox_init() {
    font = cairo_toy_font_face_create("", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    // Warm-up: force font internals to be cached
    const char *test_text = "TEST TEXT";
    cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 10, 10);
    cairo_t *cr = cairo_create(cst);
    cairo_set_font_face(cr, font);
    cairo_text_extents_t extents;
    cairo_text_extents(cr, test_text, &extents);
    cairo_show_text(cr, test_text);
    cairo_destroy(cr);
    cairo_surface_destroy(cst);
}
```

`map_draw_labels()` now calls `cairo_set_font_face(cr, font)` directly, using
the already-cached face.  The corresponding `cairo_font_face_destroy(font)` at
the end of `map_draw_labels()` was removed.

`map_pre_sandbox_init()` is declared in `gtk-v2/src/gtk2proto.h` so `main.c`
can call it without a forward declaration.

### config.h output path

```diff
-configure_file("config.h.in" "config.h")
+configure_file(
+    "${PROJECT_SOURCE_DIR}/config.h.in"
+    "${PROJECT_BINARY_DIR}/config.h"
+)
```

The generated `config.h` now lands in the CMake binary directory rather than
the source tree, following modern CMake practice and keeping the source tree
clean.

---

## Color Type: `GdkColor` to `GdkRGBA`

### Why this changed

`GdkColor` is a 16-bit-per-channel integer color type (`red`/`green`/`blue` in
the range 0-65535).  It was deprecated in GTK 3.14 and is absent from GTK 4.
GTK3 uses `GdkRGBA`, a four-channel float type (`red`/`green`/`blue`/`alpha`
each 0.0-1.0).

### Files changed

**`gtk-v2/src/main.h`**

```diff
-extern GdkColor root_color[NUM_COLORS];
+extern GdkRGBA  root_color[NUM_COLORS];
```

`root_color` is the palette used by `magicmap.c` to colour map tiles.  The
array declaration in the header drives every downstream consumer.

**`gtk-v2/src/main.c`**

```diff
-GdkColor root_color[NUM_COLORS];
+GdkRGBA  root_color[NUM_COLORS];
 ...
-gdk_color_parse(colorname[i], &root_color[i]);
+gdk_rgba_parse(&root_color[i], colorname[i]);
```

`gdk_color_parse` has the signature `(const char*, GdkColor*)`.
`gdk_rgba_parse` reverses the argument order to `(GdkRGBA*, const char*)`.

---

## Cairo Drawing: `gdk_cairo_create` Removal

### Why this changed

`gdk_cairo_create(GdkWindow*)` was the GTK2 way to obtain a Cairo context for
drawing.  It was deprecated in GTK 3.22 because it is incompatible with the GTK3
rendering model.

### Map drawing (`gtk-v2/src/map.c`)

The map renderer builds a `cairo_image_surface_t` offline and then needs to
blit it to the screen.

**New pattern:**

```c
// Module-level:
static cairo_surface_t *map_surface = NULL;

// Inside gtk_map_redraw():
if (map_surface) { cairo_surface_destroy(map_surface); }
map_surface = cst;
gtk_widget_queue_draw(map_drawing_area);

// New draw signal handler:
static gboolean map_expose_event(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    if (!map_surface) { return FALSE; }
    float scale = use_config[CONFIG_MAPSCALE] / 100.0;
    if (use_config[CONFIG_MAPSCALE] != 100) { cairo_scale(cr, scale, scale); }
    cairo_set_source_surface(cr, map_surface, 0, 0);
    if (use_config[CONFIG_MAPSCALE] % 100 == 0) {
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    }
    cairo_paint(cr);
    return FALSE;
}
```

### Magic map drawing (`gtk-v2/src/magicmap.c`)

- `draw_magic_map()` now only calls `gtk_widget_queue_draw(magic_map)`.
- `on_drawingarea_magic_map_expose_event(GtkWidget *widget, cairo_t *cr)`
  is the draw signal handler.

The signal connection in `main.c` was updated:

```diff
-g_signal_connect(magic_map, "expose_event",
-    G_CALLBACK(on_drawingarea_magic_map_expose_event), NULL);
+g_signal_connect(magic_map, "draw",
+    G_CALLBACK(on_drawingarea_magic_map_expose_event), NULL);
```

---

## Layout Widgets: `GtkTable` to `GtkGrid`

### Why this changed

`GtkTable` was deprecated in GTK 3.4 and removed in GTK 4.  `GtkGrid` is the
direct replacement.

| GtkTable | GtkGrid |
|----------|---------|
| `gtk_table_attach(table, child, left, right, top, bottom, xopts, yopts, xpad, ypad)` | `gtk_grid_attach(grid, child, col, row, width, height)` |
| `gtk_table_resize(table, rows, cols)` | Not needed - GtkGrid auto-resizes |
| `gtk_table_get_size(table, &rows, &cols)` | `gtk_grid_get_child_at(grid, col, row)` loop |

### Source files changed

**`gtk-v2/src/info.c`**, **`gtk-v2/src/stats.c`**, **`gtk-v2/src/inventory.c`**

All programmatic `gtk_table_*` calls replaced with `gtk_grid_*` equivalents.

### UI files changed

All 11 layout `.ui` files had their programmatically-accessed table widgets
converted from `GtkTable` to `GtkGrid`.

---

## Widget Separator: `GtkHSeparator` to `GtkSeparator`

`GtkHSeparator` and `GtkVSeparator` were removed in GTK3.

```xml
<!-- Before -->
<object class="GtkHSeparator" id="msgctrl_hseparator_header"/>

<!-- After -->
<object class="GtkSeparator" id="msgctrl_hseparator_header">
  <property name="orientation">horizontal</property>
</object>
```

---

## Widget Alignment: `gtk_misc_set_alignment` Removal

`GtkMisc` was deprecated in GTK 3.14. Replaced with `GtkWidget` alignment API:

```diff
-gtk_misc_set_alignment(GTK_MISC(widget), 0.0f, 0.5f);
-gtk_misc_set_padding(GTK_MISC(widget), 2, 0);
+gtk_widget_set_halign(widget, GTK_ALIGN_START);
+gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
+gtk_widget_set_margin_start(widget, 2);
```

---

## Stock Items: `GTK_STOCK_*` Removal

GTK stock icons were deprecated in GTK 3.10 and removed in GTK 4.

```diff
-GTK_STOCK_YES, 1,
-GTK_STOCK_NO,  2,
+"_Yes", 1,
+"_No",  2,
```

---

## Tree View Color Columns: `GDK_TYPE_COLOR` to `GDK_TYPE_RGBA`

```diff
-gtk_tree_store_new(..., GDK_TYPE_COLOR, GDK_TYPE_COLOR, ...)
+gtk_tree_store_new(..., GDK_TYPE_RGBA,  GDK_TYPE_RGBA,  ...)
```

```diff
-gtk_tree_view_column_add_attribute(col, renderer, "background-gdk", LIST_BACKGROUND);
-gtk_tree_view_column_add_attribute(col, renderer, "foreground-gdk", LIST_FOREGROUND);
+gtk_tree_view_column_add_attribute(col, renderer, "background-rgba", LIST_BACKGROUND);
+gtk_tree_view_column_add_attribute(col, renderer, "foreground-rgba", LIST_FOREGROUND);
```

---

## Widget Background Color: `gtk_widget_modify_base/bg` to CSS Providers

`gtk_widget_modify_base` and `gtk_widget_modify_bg` were deprecated in GTK 3.0.
In GTK3, per-widget color overrides are applied through CSS providers.

### Stat bars (`gtk-v2/src/stats.c`)

```c
static void set_bar_color(GtkWidget *bar, const GdkRGBA *color) {
    static GtkCssProvider *providers[MAX_STAT_BARS];
    int idx = /* bar index */;
    if (providers[idx]) {
        gtk_style_context_remove_provider(...);
        g_object_unref(providers[idx]);
    }
    char css[128];
    snprintf(css, sizeof(css),
             "progressbar progress { background-color: rgba(%d,%d,%d,%.3f); }",
             (int)(color->red * 255), (int)(color->green * 255),
             (int)(color->blue * 255), color->alpha);
    providers[idx] = gtk_css_provider_new();
    gtk_css_provider_load_from_data(providers[idx], css, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(bar),
                                   GTK_STYLE_PROVIDER(providers[idx]),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}
```

### Inventory icon view (`gtk-v2/src/inventory.c`)

A module-level singleton provider is initialised once and added or removed
from each cell's `GtkStyleContext` only when that cell's `applied` state
actually changes.

---

## Info Pane Text Styling: `GtkStyle` to `GtkStyleContext` + CSS

`gtk_rc_get_style_by_paths` was removed in GTK3. A `GtkStyleContext` is
created programmatically for each CSS class name. The `set_text_tag_from_style`
function reads foreground and background colors from the context.

---

## Theme System: GTK RC Files to CSS

### The fix

A module-level `GtkCssProvider *theme_provider` tracks the currently-loaded
theme. A new `apply_theme_css(path)` function replaces it atomically.

`init_theme()` calls `apply_theme_css(THEME_DEFAULT)`. `load_theme()` now
calls `apply_theme_css(theme)` before invoking `*_get_styles()`.

### Application-specific CSS classes

| Class prefix | Consumer | Property |
|---|---|---|
| `.cf-stat-normal`, `.cf-stat-low`, `.cf-stat-super`, `.cf-stat-grad-*` | `stats.c` | `color` |
| `.cf-inv-magical`, `.cf-inv-cursed`, `.cf-inv-unpaid` | `inventory.c` | `color` or `background-color` |
| `.cf-spell-attuned`, `.cf-spell-repelled`, `.cf-spell-denied`, `.cf-spell-normal` | `spells.c` | `color` or `background-color` |

### Standard theme - `themes/standard.css`

Two pre-existing invalid CSS color names were fixed:

```diff
-.info_darkorange { color: darkorange2; }
+.info_darkorange { color: darkorange; }

-.msg_spell_failure { color: grey50; }
+.msg_spell_failure { color: grey; }
```

### Black theme - `themes/black.css` (new file)

A complete dark-mode CSS theme was created.

### Theme path persistence (`gtk-v2/src/config.c`)

`setup_config_dialog()` uses `gtk_file_chooser_set_current_folder()` on Windows
and `gtk_file_chooser_set_filename()` on other platforms, split via
`#ifdef _WIN32` / `#else`.

`read_config_dialog()` now passes every path returned by
`gtk_file_chooser_get_filename()` through `g_canonicalize_filename(buf, NULL)`
before storing it.

---

## Inventory and Spell Color Model: foreground + background

The old model stored only background colors. The new model stores both:

```c
static GdkRGBA inv_fg_colors[Style_Last];
static GdkRGBA inv_bg_colors[Style_Last];
static bool     inv_has_fg[Style_Last];
static bool     inv_has_bg[Style_Last];
```

---

## UI Files: GTK Version Attribute and Property Names

All 12 `.ui` files had their GtkBuilder XML updated for GTK3 compatibility.

---

## Window Titles

All 11 layout `.ui` files were given consistent titles following the pattern
`"Crossfire Client - GTK v3 - <Layout>"`.

| File | Old title | New title |
|------|-----------|-----------|
| `caelestis.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - Caelestis |
| `chthonic.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - Chthonic |
| `eureka.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - Eureka |
| `gtk-v1.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - GTK v1 |
| `gtk-v2.ui` | Crossfire Client | Crossfire Client - GTK v3 - GTK v2 |
| `lobotomy.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - Lobotomy |
| `meflin.ui` | Crossfire GTK V2 Client - Meflin | Crossfire Client - GTK v3 - Meflin |
| `oroboros.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - Oroboros |
| `sixforty.ui` | Crossfire GTK V2 Client - SixForty | Crossfire Client - GTK v3 - SixForty |
| `un-deux.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - Un-Deux |
| `v1-redux.ui` | Crossfire Client - GTK v2 | Crossfire Client - GTK v3 - V1 Redux |

---

## CI/CD (`.github/workflows/build.yml`)

A GitHub Actions workflow builds the GTK3 client on every push or pull request.
Three jobs run in parallel:

| Job | Runner | `SOUND` | `METASERVER2` | Artifact |
|-----|--------|---------|---------------|----------|
| Linux Full | `ubuntu-latest` | ON | ON | `crossfire-client-gtk3` |
| Linux Minimal | `ubuntu-latest` | OFF | OFF | - |
| Windows | `windows-latest` (MSYS2/UCRT64) | ON | ON | `crossfire-client-gtk3.exe` |

### Linux dependency fix (Ubuntu 24.04 Noble)

Ubuntu 24.04 renamed the package that provides `glib-compile-resources`. The
workflow uses `libglib2.0-dev-bin` rather than the old `libgio-2.0-dev-bin`.

### `cfsndserv.c` conditional compilation (`HAVE_SOUND`)

`cfsndserv.c` is only added to the executable via `target_sources` inside an
`if(SOUND)` block. No-op stubs for `cf_snd_init`, `cf_snd_exit`,
`cf_play_music`, and `cf_play_sound` are compiled under `#ifndef HAVE_SOUND`.

### `enable_testing()` ordering fix

`enable_testing()` must be called before any `add_subdirectory()` that contains
`add_test()` calls.

### Windows build (MSYS2/UCRT64)

The Windows job uses the `msys2/setup-msys2@v2` action with `msystem: UCRT64`.
Key naming note: the curl package in MSYS2/UCRT64 is
`mingw-w64-ucrt-x86_64-curl`, not `libcurl`. Perl is pre-installed on the
`windows-latest` runner and does not need to be listed.

### Node 24 runtime upgrade

GitHub deprecated Node 20 on Actions runners effective June 16, 2026. The
workflow was updated to use Node 24-compatible action versions:

| Action | Before | After | Notes |
|---|---|---|---|
| `actions/checkout` | `@v4` | `@v6` | Node 24 first available in v5.0.0 |
| `actions/upload-artifact` | `@v4` | `@v7` | Node 24 default from v6.0.0 |
| `msys2/setup-msys2` | `@v2` | `@v2` | Node 24 landed in v2.31.0; no tag change needed |

All three actions require a minimum Actions Runner version of v2.327.1, which
is satisfied by the GitHub-hosted `ubuntu-latest` and `windows-latest` runners.

---

## Windows/MSYS2 Compatibility Fixes (gtk3-client-performance-improvements branch)

The following fixes were made during development of the performance branch after
testing on a real MSYS2/UCRT64 Windows build. They are all Windows-specific and
do not affect Linux or macOS builds.

### `_WIN32` vs `WIN32` preprocessor macro

MSYS2/UCRT64 defines `_WIN32`, not `WIN32`. All Windows-specific guards in
`gtk-v2/src/` were updated from `#ifdef WIN32` to `#ifdef _WIN32`. The old
guards were silently skipped by the MSYS2 compiler, causing Windows-specific
code paths to never execute.

Affected files: `config.c`, `inventory.c`, `image.c`, `keys.c`, `main.c`,
`main.h`, `menubar.c`.

### `setsockopt` cast for Winsock2 (`common/client.c`)

The POSIX `setsockopt` takes `void *` for the option value. Winsock2 declares
it as `const char *`. The call site in `client.c` was passing `int *`, which
is a type mismatch on Windows. Fixed by casting to `(const char *)`.

### XPM image loader crash (`gtk-v2/src/image.c`, `gtk-v2/src/inventory.c`)

`libpixbufloader-xpm.dll` causes a fatal `cannot register existing type
'GdkPixbuf'` crash at startup on Windows due to a GObject type registration
conflict in the MSYS2/UCRT64 GTK3 build. The XPM and SVG loaders must not
be included in the Windows deployment bundle.

Code that called `gdk_pixbuf_new_from_xpm_data()` was wrapped in
`#ifdef _WIN32` / `#else` blocks. On Windows, `gdk_pixbuf_new_from_inline()`
is used instead, with inline RGBA data generated by `gdk-pixbuf-csource`:

```bash
gdk-pixbuf-csource --name=question_inline pixmaps/question.xpm \
  > gtk-v2/src/question_inline.h
```

New files: `gtk-v2/src/question_inline.h`, `gtk-v2/src/inv_pixbufs.h`
(pre-existing; the `_WIN32` guard in `inventory.c` that activates it was
corrected from `WIN32`).

### Window position save/load (`gtk-v2/src/config.c`)

`save_winpos()` and `load_window_positions()` iterate `gtk_builder_get_objects()`
and filter by `type == GTK_TYPE_PANED`. On Windows/MSYS2/UCRT64, `GtkHPaned`
and `GtkVPaned` are registered as distinct GTypes and do not match
`GTK_TYPE_PANED` exactly, so all paned widgets were silently skipped and no
panel positions were ever saved or restored.

Fixed by replacing the exact type comparison with `g_type_is_a()`:

```c
/* Before - misses GtkHPaned and GtkVPaned on Windows */
if (G_OBJECT_TYPE(widget) == GTK_TYPE_PANED)

/* After - matches GtkPaned, GtkHPaned, GtkVPaned */
if (g_type_is_a(G_OBJECT_TYPE(widget), GTK_TYPE_PANED))
```

### Theme file chooser directory (`gtk-v2/src/config.c`)

The theme file chooser (`theme_filechooser`) was using
`gtk_file_chooser_set_filename()` on Windows. The Windows native file dialog
silently ignores this call, leaving the chooser pointing at whatever directory
it last visited - in practice the `ui/` directory, since the `ui_filechooser`
above it had just navigated there.

Fixed by applying the same pattern used for the UI file chooser: on Windows,
always call `gtk_file_chooser_set_current_folder()` with the absolute themes
directory path derived from `CF_DATADIR_RT`:

```c
#ifdef _WIN32
    gchar *abs_theme_dir = g_build_filename(CF_DATADIR_RT, "themes", NULL);
    gtk_file_chooser_set_current_folder(theme_filechooser, abs_theme_dir);
    g_free(abs_theme_dir);
#else
    gtk_file_chooser_set_filename(theme_filechooser, theme);
#endif
```

---

## Files Changed Summary

| File | Change category |
|------|----------------|
| `CMakeLists.txt` | gtk+-2.0 to gtk+-3.0; config.h output path; `HAVE_CAPSICUM` detection |
| `config.h.in` | `HAVE_CAPSICUM` cmakedefine; `HAVE_SOUND` cmakedefine |
| `common/client.c` | TCP_NODELAY enabled on Windows via `#elif defined(_WIN32)` Winsock path; `setsockopt` cast fix |
| `common/mapdata.c` | `anim_sync_max` high-water mark; bounds `mapdata_animation()` SYNC scan |
| `gtk-v2/src/config.c` | CSS provider lifecycle; `apply_theme_css()`; `load_theme()` fix; theme/UI path canonicalization; file chooser pre-selection fix; memory leak fix; TCP_NODELAY live-update fix; `_WIN32` guards; `g_type_is_a()` for paned type checks; theme filechooser directory fix |
| `gtk-v2/src/gtk2proto.h` | Updated signatures; new `get_css_fg/bg_color` declarations; `map_pre_sandbox_init` |
| `gtk-v2/src/image.c` | `gtk_events_pending` spin-loop to `g_main_context_iteration(NULL, FALSE)`; XPM to inline RGBA on `_WIN32` |
| `gtk-v2/src/info.c` | `GtkStyle` to `GtkStyleContext`; CSS color helpers |
| `gtk-v2/src/inventory.c` | `GdkColor` to `GdkRGBA`; GtkTable to GtkGrid; fg+bg color model; CSS; differential store update; `GDK_BUTTON_PRESS_MASK`; shared applied-item `GtkCssProvider`; `_WIN32` guard activates inline pixbuf data |
| `gtk-v2/src/inv_pixbufs.h` | Pre-existing inline RGBA data for inventory tab icons; `_WIN32` guard corrected |
| `gtk-v2/src/question_inline.h` | New: inline RGBA data for question mark pixmap generated by `gdk-pixbuf-csource` |
| `gtk-v2/src/keys.c` | GTK_STOCK_YES/NO to mnemonic text labels; `WIN32` to `_WIN32` |
| `gtk-v2/src/magicmap.c` | draw signal; `gdk_cairo_create` removal; `GdkRGBA` colors |
| `gtk-v2/src/main.c` | `GdkRGBA` init; `expose_event` to `draw`; Capsicum sandbox support; `redraw_idle_id` guard; `my_log_handler` sleep removed; `WIN32` to `_WIN32` |
| `gtk-v2/src/main.h` | `GdkColor` to `GdkRGBA` for `root_color`; `WIN32` to `_WIN32` |
| `gtk-v2/src/menubar.c` | `WIN32` to `_WIN32` |
| `gtk-v2/src/map.c` | Persistent `cairo_surface_t`; draw signal blitting; `map_pre_sandbox_init`; static label font; dirty-region tracking; `display_mapscroll` blit; cached `lm_surface`; RGB24 surface; `OPERATOR_SOURCE`; manual bilinear darkness upscale; direct pixel OVER blend for smooth tiles |
| `gtk-v2/src/spells.c` | `GdkColor` to `GdkRGBA`; `GDK_TYPE_RGBA`; fg+bg model; CSS |
| `gtk-v2/src/stats.c` | `GdkColor` to `GdkRGBA`; GtkTable to GtkGrid; CSS bar colors |
| `gtk-v2/themes/standard.css` | cf-* application color classes; invalid color name fixes |
| `gtk-v2/themes/black.css` | New dark theme |
| `gtk-v2/ui/caelestis.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/chthonic.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/dialogs.ui` | GtkTable to GtkGrid; GtkHSeparator to GtkSeparator; `sandbox_enable` checkbox |
| `gtk-v2/ui/eureka.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/gtk-v1.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/gtk-v2.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/lobotomy.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/meflin.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/oroboros.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/sixforty.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/un-deux.ui` | GtkTable to GtkGrid; window title |
| `gtk-v2/ui/v1-redux.ui` | GtkTable to GtkGrid; window title |
| `.github/workflows/build.yml` | GitHub Actions CI: three jobs (Linux full, Linux minimal, Windows MSYS2/UCRT64); `libglib2.0-dev-bin` Noble fix; Windows artifact |
| `gtk-v2/src/CMakeLists.txt` | `cfsndserv.c` moved to `if(SOUND) target_sources(...)` block |
| `gtk-v2/src/sound.c` | `#ifndef HAVE_SOUND` no-op stubs for `cf_snd_*` functions |
| `gtk-v2/win32/client.nsi` | Updated for GTK3: exe name, PROGRAMFILES64, Start Menu folder, registry keys |
| `gtk-v2/test-windows-smoke.sh` | New: 13-check smoke test for Windows build and deploy correctness |