# GTK3 Port Changes

This document describes every source-level change made to port the Crossfire
GTK client from GTK2 to GTK3, the rationale behind each decision, and any
design constraints that shaped the implementation.  It is intended for
developers who need to understand, maintain, or extend this codebase.

---

## Background

The GTK client (`gtk-v2/`) was written against GTK2 and accumulated a large
number of APIs that were deprecated across GTK 3.0–3.22 and finally removed in
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

**`gtk-v2/ui/dialogs.ui`** — An "Enable Sandbox" `GtkCheckButton` with
`id="sandbox_enable"` was added to the metaserver dialog's button row, before
the Connect button.  The widget's `sensitive` property defaults to `False` in
the UI file; `init_ui()` in `main.c` overrides this based on `HAVE_CAPSICUM`.

**`gtk-v2/src/main.c`** — Three changes:

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

After the event loop returns, the loop also breaks if sandboxing was active —
reconnecting would require filesystem access that the sandbox no longer permits:

```c
if (sandbox_enabled) {
    break;
}
```

**`gtk-v2/src/map.c`** — `map_draw_labels()` previously created and destroyed a
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

## Color Type: `GdkColor` → `GdkRGBA`

### Why this changed

`GdkColor` is a 16-bit-per-channel integer color type (`red`/`green`/`blue` in
the range 0–65535).  It was deprecated in GTK 3.14 and is absent from GTK 4.
GTK3 uses `GdkRGBA`, a four-channel float type (`red`/`green`/`blue`/`alpha`
each 0.0–1.0).

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

**`gtk-v2/src/stats.c`** — see *Stat Bars* section below.

**`gtk-v2/src/inventory.c`** — see *Inventory* section below.

**`gtk-v2/src/spells.c`** — see *Spells* section below.

---

## Cairo Drawing: `gdk_cairo_create` Removal

### Why this changed

`gdk_cairo_create(GdkWindow*)` was the GTK2 way to obtain a Cairo context for
drawing.  It was deprecated in GTK 3.22 because it is incompatible with the GTK3
rendering model: GTK3 composites widgets through a scene graph and provides a
`cairo_t` to each widget's `draw` signal handler, already clipped and
transformed.  Calling `gdk_cairo_create` outside a `draw` handler is undefined
and produces visual corruption or crashes under hardware-accelerated backends.

### Map drawing (`gtk-v2/src/map.c`)

The map renderer builds a `cairo_image_surface_t` offline (in
`gtk_map_redraw()`) and then needs to blit it to the screen.

**Old pattern:**

```c
// Inside gtk_map_redraw():
cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(map_drawing_area));
cairo_set_source_surface(cr, cst, 0, 0);
cairo_paint(cr);
cairo_destroy(cr);
```

This called `gdk_cairo_create` synchronously from non-signal code, which is
illegal in GTK3.

**New pattern:**

```c
// Module-level:
static cairo_surface_t *map_surface = NULL;

// Inside gtk_map_redraw():
if (map_surface) { cairo_surface_destroy(map_surface); }
map_surface = cst;           // take ownership of the new surface
gtk_widget_queue_draw(map_drawing_area);  // schedule a draw signal

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

The module retains ownership of `map_surface` across frames.  When a new frame
is ready, the old surface is destroyed, the new one is stored, and a draw is
requested.  GTK then calls `map_expose_event` with a valid `cairo_t` at the
correct time.

**Important:** `map_expose_event` must not call `draw_map()` or
`gtk_map_redraw()`.  Doing so from inside a draw signal creates infinite
recursion.

### Magic map drawing (`gtk-v2/src/magicmap.c`)

The magic map was previously drawn entirely in `draw_magic_map()`, including
a `gdk_cairo_create` call.

**New split:**

- `draw_magic_map()` now only updates `cpl.mapxres`/`cpl.mapyres` from the
  widget's allocated size (via `gtk_widget_get_allocated_width/height`) and
  calls `gtk_widget_queue_draw(magic_map)`.
- `magic_map_flash_pos()` also calls `gtk_widget_queue_draw(magic_map)`.
- `on_drawingarea_magic_map_expose_event(GtkWidget *widget, cairo_t *cr)`
  is the draw signal handler.  It receives the context from GTK and renders
  all tiles using `gdk_cairo_set_source_rgba` (replacing the deprecated
  `gdk_cairo_set_source_color`).

The signal connection in `main.c` was updated accordingly:

```diff
-g_signal_connect(magic_map, "expose_event",
-    G_CALLBACK(on_drawingarea_magic_map_expose_event), NULL);
+g_signal_connect(magic_map, "draw",
+    G_CALLBACK(on_drawingarea_magic_map_expose_event), NULL);
```

The GTK2 signal was `expose_event`; the GTK3 signal is `draw`.  The handler
signature changed from `(GtkWidget*, GdkEventExpose*, gpointer)` to
`(GtkWidget*, cairo_t*, gpointer)`.

---

## Layout Widgets: `GtkTable` → `GtkGrid`

### Why this changed

`GtkTable` was deprecated in GTK 3.4 and removed in GTK 4.  `GtkGrid` is the
direct replacement.  The key API differences:

| GtkTable | GtkGrid |
|----------|---------|
| `gtk_table_attach(table, child, left, right, top, bottom, xopts, yopts, xpad, ypad)` | `gtk_grid_attach(grid, child, col, row, width, height)` |
| `gtk_table_resize(table, rows, cols)` | Not needed — GtkGrid auto-resizes |
| `gtk_table_get_size(table, &rows, &cols)` | `gtk_grid_get_child_at(grid, col, row)` loop |

### Source files changed

**`gtk-v2/src/info.c`** (`msgctrl_init`)

The message-control dialog builds its table programmatically.  The code that
previously called `gtk_table_get_size`, `gtk_table_resize`, and
`gtk_table_attach_defaults` was replaced with a `GtkGrid` equivalent.

Counting pre-existing rows in a `GtkGrid` requires a manual scan because
`GtkGrid` has no `get_size` API:

```c
title_rows = 0;
while (gtk_grid_get_child_at(grid, 0, title_rows) != NULL) { title_rows++; }
```

Each new widget is then attached with:

```c
gtk_grid_attach(grid, widget, 0, row, 1, 1);
```

**`gtk-v2/src/stats.c`** (`stats_init`)

The skill-experience and protection grids were populated using
`gtk_table_attach` with `GTK_EXPAND|GTK_FILL` options.  These were replaced
with `gtk_grid_attach` calls using uniform 1×1 cell spans.

**`gtk-v2/src/inventory.c`** (`draw_icon_view`)

`gtk_table_resize` calls were removed (GtkGrid handles this automatically).
`gtk_table_attach` calls were replaced with `gtk_grid_attach`.

### UI files changed

All 11 layout `.ui` files had their programmatically-accessed table widgets
converted from `GtkTable` to `GtkGrid`.  The affected widget IDs are:
`inv_table`, `table_protections`, `table_skills_exp`.

`dialogs.ui` additionally had `msgctrl_table` and `msgctrl_table_parameters`
converted.

The GtkBuilder XML packing attribute format changed:

**GtkTable child packing (old):**
```xml
<packing>
  <property name="left_attach">0</property>
  <property name="right_attach">1</property>
  <property name="top_attach">0</property>
  <property name="bottom_attach">1</property>
  <property name="x_options">fill</property>
  <property name="y_options"/>
</packing>
```

**GtkGrid child packing (new):**
```xml
<packing>
  <property name="left_attach">0</property>
  <property name="top_attach">0</property>
  <property name="width">1</property>
  <property name="height">1</property>
</packing>
```

Padding that was expressed as `x_padding`/`y_padding` packing attributes was
moved to `margin_start`/`margin_top`/`margin_bottom` widget properties.

A Python script was used to bulk-convert the 11 layout files, handling both
underscore (`n_rows`) and hyphen (`n-rows`) property name variants that
appeared across different Glade versions.

---

## Widget Separator: `GtkHSeparator` → `GtkSeparator`

`GtkHSeparator` and `GtkVSeparator` were removed in GTK3.  The replacement is
`GtkSeparator` with an explicit orientation property.

In `dialogs.ui`:

```xml
<!-- Before -->
<object class="GtkHSeparator" id="msgctrl_hseparator_header"/>

<!-- After -->
<object class="GtkSeparator" id="msgctrl_hseparator_header">
  <property name="orientation">horizontal</property>
</object>
```

---

## Widget Alignment: `gtk_misc_set_alignment` / `gtk_misc_set_padding` Removal

`GtkMisc` (the base class providing alignment and padding for `GtkLabel` and
`GtkImage`) was deprecated in GTK 3.14.  The alignment properties are now set
directly on the widget using the standard `GtkWidget` API.

In `gtk-v2/src/info.c` (`msgctrl_init`):

```diff
-gtk_misc_set_alignment(GTK_MISC(widget), 0.0f, 0.5f);
-gtk_misc_set_padding(GTK_MISC(widget), 2, 0);
+gtk_widget_set_halign(widget, GTK_ALIGN_START);
+gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
+gtk_widget_set_margin_start(widget, 2);
```

---

## Stock Items: `GTK_STOCK_*` Removal

GTK stock icons and labels (`GTK_STOCK_YES`, `GTK_STOCK_NO`, etc.) were
deprecated in GTK 3.10 and removed in GTK 4.

In `gtk-v2/src/keys.c` (`keybind_overwrite_confirm`), the confirmation dialog
buttons were using stock labels:

```diff
-GTK_STOCK_YES, 1,
-GTK_STOCK_NO,  2,
+"_Yes", 1,
+"_No",  2,
```

The underscore prefix marks the keyboard accelerator character, matching the
GTK convention for mnemonic labels.

---

## Tree View Color Columns: `GDK_TYPE_COLOR` → `GDK_TYPE_RGBA`

`GtkTreeStore` and `GtkListStore` column types that stored color values had to
be updated from the GTK2 boxed type to the GTK3 equivalent.

In `gtk-v2/src/inventory.c` and `gtk-v2/src/spells.c`:

```diff
-gtk_tree_store_new(..., GDK_TYPE_COLOR, GDK_TYPE_COLOR, ...)
+gtk_tree_store_new(..., GDK_TYPE_RGBA,  GDK_TYPE_RGBA,  ...)
```

The corresponding `GtkCellRendererText` column attribute names also changed:

```diff
-gtk_tree_view_column_add_attribute(col, renderer, "background-gdk", LIST_BACKGROUND);
-gtk_tree_view_column_add_attribute(col, renderer, "foreground-gdk", LIST_FOREGROUND);
+gtk_tree_view_column_add_attribute(col, renderer, "background-rgba", LIST_BACKGROUND);
+gtk_tree_view_column_add_attribute(col, renderer, "foreground-rgba", LIST_FOREGROUND);
```

---

## Widget Background Color: `gtk_widget_modify_base/bg` → CSS Providers

`gtk_widget_modify_base` and `gtk_widget_modify_bg` were deprecated in GTK 3.0.
In GTK3, per-widget color overrides are applied through CSS providers.

### Stat bars (`gtk-v2/src/stats.c`)

The progress bars that display HP, SP, grace, food, and experience are colored
dynamically based on current versus maximum values.  A module-level array of
`GtkCssProvider*` (one per stat bar) is maintained:

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

The CSS selector `progressbar progress` targets the fill trough of a
`GtkProgressBar`, not the track behind it.

### Inventory icon view (`gtk-v2/src/inventory.c`)

The icon view uses a `GtkDrawingArea` per item grid cell.  Applied items need
a grey background.  A module-level singleton provider is initialised once:

```c
static GtkCssProvider *applied_css_provider = NULL;

// Lazy init at first call to draw_inv_table():
if (applied_css_provider == NULL) {
    applied_css_provider = gtk_css_provider_new();
    snprintf(buf, sizeof(buf),
             "* { background-color: rgba(%d,%d,%d,%.3f); }",
             (int)(applied_color.red   * 255),
             (int)(applied_color.green * 255),
             (int)(applied_color.blue  * 255),
             applied_color.alpha);
    gtk_css_provider_load_from_data(applied_css_provider, buf, -1, NULL);
}
```

The provider is added or removed from each cell's `GtkStyleContext` only when
that cell's `applied` state actually changes, tracked via a boolean sentinel
stored in `g_object_set_data`:

```c
gboolean was_applied = GPOINTER_TO_INT(
    g_object_get_data(G_OBJECT(cell), "inv-applied"));
if (was_applied && !tmp->applied) {
    gtk_style_context_remove_provider(ctx,
            GTK_STYLE_PROVIDER(applied_css_provider));
    g_object_set_data(G_OBJECT(cell), "inv-applied", NULL);
} else if (!was_applied && tmp->applied) {
    gtk_style_context_add_provider(ctx,
            GTK_STYLE_PROVIDER(applied_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set_data(G_OBJECT(cell), "inv-applied", GINT_TO_POINTER(1));
}
```

This avoids allocating and freeing a `GtkCssProvider` on every redraw cycle.

---

## Info Pane Text Styling: `GtkStyle` → `GtkStyleContext` + CSS

### Why this changed

The old `info.c` used `gtk_rc_get_style_by_paths`, `GtkStyle`, and
`GdkColor`-based tag assignment to apply theme colors to the info pane text
buffers.  `gtk_rc_get_style_by_paths` was removed in GTK3 (the entire GTK RC
file system was replaced by CSS).

### New approach

A `GtkStyleContext` is created programmatically for each CSS class name
(e.g. `"info_red"`, `"msg_book"`):

```c
GtkWidgetPath *path = gtk_widget_path_new();
gtk_widget_path_append_type(path, GTK_TYPE_TEXT_VIEW);
GtkStyleContext *sc = gtk_style_context_new();
gtk_style_context_set_path(sc, path);
gtk_style_context_add_class(sc, style_name);
```

The `set_text_tag_from_style` function reads foreground and background colors
from the context and compares them against a base style context (no CSS
classes).  If they differ, the corresponding text tag property is set:

```c
void set_text_tag_from_style(GtkTextTag *tag,
                              GtkStyleContext *sc,
                              GtkStyleContext *base_style) {
    GdkRGBA fg, bg, bfg, bbg;
    gtk_style_context_get_color(sc,   GTK_STATE_FLAG_NORMAL, &fg);
    gtk_style_context_get_color(base, GTK_STATE_FLAG_NORMAL, &bfg);
    gtk_style_context_get_background_color(sc,   GTK_STATE_FLAG_NORMAL, &bg);
    gtk_style_context_get_background_color(base, GTK_STATE_FLAG_NORMAL, &bbg);
    if (!gdk_rgba_equal(&fg, &bfg))
        g_object_set(tag, "foreground-rgba", &fg, NULL);
    if (!gdk_rgba_equal(&bg, &bbg))
        g_object_set(tag, "background-rgba", &bg, NULL);
}
```

`gtk_style_context_get_background_color` is deprecated in GTK 3.16 but remains
functional in 3.24; it is retained here because there is no clean replacement
for reading back a computed `background-color` value in GTK3.  The build uses
`-Wno-deprecated-declarations` to suppress the warning.

### Function signature change

```diff
-void add_style_to_textbuffer(Info_Pane *pane, GtkStyle *base_style)
+void add_style_to_textbuffer(Info_Pane *pane, void *unused)
```

The second parameter was always passed as `NULL` at the call site.  The type
was updated to `void *` to reflect this and eliminate the `GtkStyle` reference.
The corresponding declaration in `gtk2proto.h` was updated to match.

---

## Theme System: GTK RC Files → CSS

### The problem

The original theme system loaded GTK2 RC files (`themes/Standard`,
`themes/Black`) using `gtk_rc_get_style_by_paths`.  This API does not exist in
GTK3.  The RC format itself was replaced by CSS in GTK 3.0.

During the initial migration, the `*_get_styles()` functions were rewritten with
hardcoded colors matching the Standard theme, and `load_theme()` was left
calling those functions without loading any CSS.  This meant:

1. Theme switching in the configuration dialog had no effect.
2. The Black theme was completely non-functional.
3. `inventory_get_styles()` and `spell_get_styles()` had one-shot guards
   (`inv_styles_init`, `spell_styles_init`, `has_init`) that prevented
   re-initialization when a new theme was selected.

### The fix

#### CSS provider lifecycle (`gtk-v2/src/config.c`)

A module-level `GtkCssProvider *theme_provider` tracks the currently-loaded
theme.  A new `apply_theme_css(path)` function replaces it atomically:

```c
static void apply_theme_css(const char *path) {
    GdkScreen *screen = gdk_screen_get_default();
    if (theme_provider) {
        gtk_style_context_remove_provider_for_screen(screen, ...);
        g_object_unref(theme_provider);
        theme_provider = NULL;
    }
    theme_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(theme_provider, path, &error);
    gtk_style_context_add_provider_for_screen(screen,
        GTK_STYLE_PROVIDER(theme_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
}
```

`init_theme()` calls `apply_theme_css(THEME_DEFAULT)`.  `load_theme()` now
calls `apply_theme_css(theme)` before invoking `*_get_styles()`, ensuring CSS
is active when the style functions read colors.

#### CSS color lookup helpers (`gtk-v2/src/info.c`)

Two exported helpers allow any module to read a color from a named CSS class
without being coupled to the info pane internals:

```c
gboolean get_css_fg_color(const char *class_name, GdkRGBA *out);
gboolean get_css_bg_color(const char *class_name, GdkRGBA *out);
```

Both create a transient `GtkStyleContext` with the named class, compare the
resulting color against the default widget style, and return `TRUE` if the
class overrides that property.  Providers added with
`gtk_style_context_add_provider_for_screen` are inherited by all contexts, so
the current theme CSS is always consulted.

#### Application-specific CSS classes

The stat bar, inventory, and spell color systems were previously embedded
directly in the C code.  They are now expressed as named CSS classes in the
theme file:

| Class prefix | Consumer | Property |
|---|---|---|
| `.cf-stat-normal`, `.cf-stat-low`, `.cf-stat-super`, `.cf-stat-grad-*` | `stats.c` | `color` (bar fill color) |
| `.cf-inv-magical`, `.cf-inv-cursed`, `.cf-inv-unpaid` | `inventory.c` | `color` or `background-color` |
| `.cf-spell-attuned`, `.cf-spell-repelled`, `.cf-spell-denied`, `.cf-spell-normal` | `spells.c` | `color` or `background-color` |

Using `color` (foreground) for stat bar fill avoids confusion with the progress
bar track background; the value is consumed as a fill color by `set_bar_color()`
regardless of its CSS semantic.

#### Standard theme — `themes/standard.css`

The existing `standard.css` (which handled info pane text colors) was extended
with the application color classes.  Two pre-existing invalid CSS color names
were also fixed:

```diff
-.info_darkorange { color: darkorange2; }  /* not a CSS color */
+.info_darkorange { color: darkorange; }

-.msg_spell_failure { color: grey50; }     /* not a CSS color */
+.msg_spell_failure { color: grey; }
```

`darkorange2` and `grey50` are X11/GTK2 color names with no CSS equivalent.
GTK3's CSS parser silently drops rules with unknown color values.

#### Black theme — `themes/black.css` (new file)

A complete dark-mode CSS theme was created.  It sets dark backgrounds on
`textview` and `treeview` widgets and defines the full set of info text and
message-type color classes adjusted for dark backgrounds.

The Black theme uses `color` (foreground text) for inventory and spell
highlights rather than `background-color`, matching the original GTK2 RC
file's `text[NORMAL]` behaviour.  The C code handles both cases: if
`get_css_bg_color` returns a color it is applied as row background; if
`get_css_fg_color` returns a color it is applied as row text color.

#### Startup ordering

The `*_get_styles()` functions are called twice during startup:

1. From `inventory_init()` / `stats_init()` — before CSS is loaded.  These
   calls use the hardcoded fallback colors.
2. From `load_theme(TRUE)` — after `init_theme()` loads the CSS.  These calls
   read the actual theme colors.

The one-shot guards were therefore removed.  `stats_get_styles()` frees and
reallocates `bar_colors` on every call.  `inventory_get_styles()` and
`spell_get_styles()` reinitialize their color arrays in-place.

#### Theme path persistence (`gtk-v2/src/config.c`)

`setup_config_dialog()` uses `gtk_file_chooser_set_current_folder()` on Windows
and `gtk_file_chooser_set_filename()` on other platforms, split via
`#ifdef WIN32` / `#else`.  On Windows, the native file dialog silently ignores
`set_filename()`, so `set_current_folder()` with an absolute path built from
`CF_DATADIR_RT` is the only reliable way to control where the dialog opens.
On non-Windows, `set_filename()` is used so the currently-active file is
pre-selected in the chooser.

`read_config_dialog()` now passes every path returned by
`gtk_file_chooser_get_filename()` through `g_canonicalize_filename(buf, NULL)`
before storing it in `theme` or `window_xml_file`.  GTK documents that
`get_filename()` returns an absolute path, but this guarantee can break on
Windows when the chooser was initialised with a relative path.  Canonicalising
at read time ensures `save_defaults()` always writes an absolute path to
`client.ini`, which is then reliably reloadable on the next launch regardless of
the working directory.

A memory leak was also fixed: when the user opened the dialog and clicked Apply
without changing the theme, the string returned by `get_filename()` was not
freed.

---

## Inventory and Spell Color Model: foreground + background

### Inventory (`gtk-v2/src/inventory.c`)

The old model stored only background colors (`inv_bg_colors[Style_Last]`).  The
new model stores both:

```c
static GdkRGBA inv_fg_colors[Style_Last];
static GdkRGBA inv_bg_colors[Style_Last];
static bool     inv_has_fg[Style_Last];
static bool     inv_has_bg[Style_Last];
```

`add_object_to_store()` sets both `LIST_FOREGROUND` and `LIST_BACKGROUND` based
on which colors are present:

```c
if (inv_has_bg[style_idx]) { background = &inv_bg_colors[style_idx]; }
if (inv_has_fg[style_idx]) { foreground = &inv_fg_colors[style_idx]; }
```

### Spells (`gtk-v2/src/spells.c`)

The same dual-color model was applied to the spell list.  In
`update_spell_information()`, the spell-path color key eventboxes are updated
differently depending on which color property is set:

- **Background set (Standard theme):** `"* { background-color: rgba(...); }"`
  applied to the eventbox widget.
- **Foreground set (Black theme):** `"label { color: rgba(...); }"` applied to
  the eventbox widget, coloring the label text instead of the box background.

---

## UI Files: GTK Version Attribute and Property Names

All 12 `.ui` files had their GtkBuilder XML updated for GTK3 compatibility.

### Toolkit version

```xml
<!-- Before -->
<interface>

<!-- After -->
<interface>
  <!-- GtkBuilder automatically uses the linked GTK version -->
```

In practice the version was set to `3.0` in the file headers by the Glade
conversion pass.

### Deprecated `GtkObject` → `GtkWidget` properties

Several layout-specific attributes that referenced `GtkObject` (removed in
GTK3) were cleaned up by the bulk conversion pass.

---

## Startup Sequence Cleanup (`gtk-v2/src/main.c`)

### Signal name: `expose_event` → `draw`

```diff
-g_signal_connect(magic_map, "expose_event",
-    G_CALLBACK(on_drawingarea_magic_map_expose_event), NULL);
+g_signal_connect(magic_map, "draw",
+    G_CALLBACK(on_drawingarea_magic_map_expose_event), NULL);
```

The `expose_event` signal was replaced by `draw` in GTK3.  The handler
signature changed (see *Cairo Drawing* section above).

### Color initialisation

The `gdk_colormap_alloc_color` call (which required a `GdkColormap`, removed
in GTK3) was replaced by a simple loop over `gdk_rgba_parse`:

```c
for (i = 0; i < NUM_COLORS; i++) {
    if (!gdk_rgba_parse(&root_color[i], colorname[i])) {
        fprintf(stderr, "gdk_rgba_parse failed (%s)\n", colorname[i]);
    }
}
```

---

## Window Titles

All 11 layout `.ui` files were given consistent, descriptive window titles
following the pattern `"Crossfire Client - GTK v3 - <Layout>"`.  Previously
the titles were inconsistent: some said `"Crossfire Client - GTK v2"`, one said
`"Crossfire Client"`, and two had non-standard formats.

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

A GitHub Actions workflow builds the GTK3 client on every push or pull request
to the `gtk3` and `master` branches.  Three jobs run in parallel:

| Job | Runner | `SOUND` | `METASERVER2` | Artifact |
|-----|--------|---------|---------------|----------|
| Linux Full | `ubuntu-latest` | ON | ON | `crossfire-client-gtk3` |
| Linux Minimal | `ubuntu-latest` | OFF | OFF | — |
| Windows | `windows-latest` (MSYS2/UCRT64) | ON | ON | `crossfire-client-gtk3.exe` |

The live-network `metaserver` ctest is excluded (`-E metaserver`) on Linux
because it requires the Crossfire metaserver to be reachable and does not
assert any correctness condition.  The Windows job has no test step because
`ctest` requires an X display that is not available on the runner.

### Linux dependency fix (Ubuntu 24.04 Noble)

Ubuntu 24.04 renamed the package that provides `glib-compile-resources`.  The
workflow uses `libglib2.0-dev-bin` (the current name) rather than the old
`libgio-2.0-dev-bin` which no longer exists on Noble.

### `cfsndserv.c` conditional compilation (`HAVE_SOUND`)

`cfsndserv.c` unconditionally includes `<SDL.h>`.  When `SOUND=OFF` the SDL
headers are not installed, causing the Minimal build to fail at compile time.
The fix has two parts:

1. `gtk-v2/src/CMakeLists.txt` — `cfsndserv.c` is only added to the executable
   via `target_sources` inside an `if(SOUND)` block instead of being listed
   unconditionally in `add_executable`.

2. `gtk-v2/src/sound.c` — no-op stubs for `cf_snd_init`, `cf_snd_exit`,
   `cf_play_music`, and `cf_play_sound` are compiled under `#ifndef HAVE_SOUND`
   so the rest of the client can link regardless of whether sound is enabled.

`HAVE_SOUND` is set in `CMakeLists.txt` inside the `if(SOUND)` block and
written to `config.h` via `#cmakedefine HAVE_SOUND` in `config.h.in`.

### `enable_testing()` ordering fix

`enable_testing()` must be called before any `add_subdirectory()` that contains
`add_test()` calls, otherwise CMake silently skips test registration and `ctest`
reports 0 tests.  The call was moved to appear before `add_subdirectory(common)`
and `add_subdirectory(gtk-v2)`.

### Windows build (MSYS2/UCRT64)

The Windows job uses the `msys2/setup-msys2@v2` action with `msystem: UCRT64`
and installs the full set of `mingw-w64-ucrt-x86_64-*` packages needed for a
sound + metaserver build.  Key naming note: the curl package in MSYS2/UCRT64 is
`mingw-w64-ucrt-x86_64-curl`, not `libcurl`.  Perl is pre-installed on the
`windows-latest` runner and does not need to be listed.

---

## Files Changed Summary

| File | Change category |
|------|----------------|
| `CMakeLists.txt` | gtk+-2.0 → gtk+-3.0; config.h output path; `HAVE_CAPSICUM` detection |
| `config.h.in` | `HAVE_CAPSICUM` cmakedefine; `HAVE_SOUND` cmakedefine |
| `common/client.c` | TCP_NODELAY enabled on Windows via `#elif defined(WIN32)` Winsock path |
| `common/mapdata.c` | `anim_sync_max` high-water mark; bounds `mapdata_animation()` SYNC scan |
| `gtk-v2/src/config.c` | CSS provider lifecycle; `apply_theme_css()`; `load_theme()` fix; theme/UI path canonicalization; file chooser pre-selection fix; memory leak fix; TCP_NODELAY live-update fix (correct fd extraction + `#include <gio/gnetworking.h>`) |
| `gtk-v2/src/gtk2proto.h` | Updated signatures; new `get_css_fg/bg_color` declarations; `map_pre_sandbox_init` |
| `gtk-v2/src/image.c` | `gtk_events_pending` spin-loop → `g_main_context_iteration(NULL, FALSE)` |
| `gtk-v2/src/info.c` | `GtkStyle` → `GtkStyleContext`; CSS color helpers |
| `gtk-v2/src/inventory.c` | `GdkColor` → `GdkRGBA`; GtkTable → GtkGrid; fg+bg color model; CSS; differential store update; `GDK_BUTTON_PRESS_MASK`; shared applied-item `GtkCssProvider` |
| `gtk-v2/src/keys.c` | GTK_STOCK_YES/NO → mnemonic text labels |
| `gtk-v2/src/magicmap.c` | draw signal; `gdk_cairo_create` removal; `GdkRGBA` colors |
| `gtk-v2/src/main.c` | `GdkRGBA` init; `expose_event` → `draw`; Capsicum sandbox support; `redraw_idle_id` guard; `my_log_handler` sleep removed |
| `gtk-v2/src/main.h` | `GdkColor` → `GdkRGBA` for `root_color` |
| `gtk-v2/src/map.c` | Persistent `cairo_surface_t`; draw signal blitting; `map_pre_sandbox_init`; static label font; dirty-region tracking; `display_mapscroll` blit; cached `lm_surface`; RGB24 surface; `OPERATOR_SOURCE`; manual bilinear darkness upscale; direct pixel OVER blend for smooth tiles |
| `gtk-v2/src/spells.c` | `GdkColor` → `GdkRGBA`; `GDK_TYPE_RGBA`; fg+bg model; CSS |
| `gtk-v2/src/stats.c` | `GdkColor` → `GdkRGBA`; GtkTable → GtkGrid; CSS bar colors |
| `gtk-v2/themes/standard.css` | cf-* application color classes; invalid color name fixes |
| `gtk-v2/themes/black.css` | New dark theme |
| `gtk-v2/ui/caelestis.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/chthonic.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/dialogs.ui` | GtkTable → GtkGrid; GtkHSeparator → GtkSeparator; `sandbox_enable` checkbox |
| `gtk-v2/ui/eureka.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/gtk-v1.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/gtk-v2.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/lobotomy.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/meflin.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/oroboros.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/sixforty.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/un-deux.ui` | GtkTable → GtkGrid; window title |
| `gtk-v2/ui/v1-redux.ui` | GtkTable → GtkGrid; window title |
| `.github/workflows/build.yml` | GitHub Actions CI: three jobs (Linux full, Linux minimal, Windows MSYS2/UCRT64); `libglib2.0-dev-bin` Noble fix; Windows artifact |
| `gtk-v2/src/CMakeLists.txt` | `cfsndserv.c` moved to `if(SOUND) target_sources(...)` block |
| `gtk-v2/src/sound.c` | `#ifndef HAVE_SOUND` no-op stubs for `cf_snd_*` functions |
