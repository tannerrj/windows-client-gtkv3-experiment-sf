# Development Comparison

Comparison of:
- **Upstream**: <https://sourceforge.net/p/crossfire/crossfire-client/ci/gtk3/tree/> — SourceForge GTK3 port branch, version 1.75.5
- **Fork**: <https://github.com/tannerrj/windows-client-gtkv3-experiment-sf> — Windows packaging fork, version 1.75.3, branched from upstream at commit `4285a62`

The fork's goal is a self-contained Windows installer built from the GTK3 port while keeping all changes non-breaking on Linux/macOS.

---

## Major Code Changes

### GTK2 → GTK3 API Migration (both repos, fork has more coverage)

Both repos perform the core GTK2-to-GTK3 migration. The fork documents this in detail in `GTK3_PORT_CHANGES.md`. Key changes:

| GTK2 API | GTK3 Replacement | Files |
|---|---|---|
| `GdkColor` / `gdk_color_parse` | `GdkRGBA` / `gdk_rgba_parse` | `main.h`, `main.c`, `stats.c`, `inventory.c`, `spells.c` |
| `gdk_cairo_create(GdkWindow*)` | persistent `cairo_surface_t` + `draw` signal handler | `map.c`, `magicmap.c` |
| `GtkTable` + `gtk_table_attach` | `GtkGrid` + `gtk_grid_attach` | `info.c`, `stats.c`, `inventory.c`, all 12 `.ui` files |
| `GtkHSeparator` / `GtkVSeparator` | `GtkSeparator` with `orientation` property | `dialogs.ui` |
| `gtk_misc_set_alignment` / `gtk_misc_set_padding` | `gtk_widget_set_halign` / `gtk_widget_set_margin_start` | `info.c` |
| `GTK_STOCK_YES` / `GTK_STOCK_NO` | `"_Yes"` / `"_No"` mnemonic labels | `keys.c` |
| `GDK_TYPE_COLOR` in tree stores | `GDK_TYPE_RGBA` | `inventory.c`, `spells.c` |
| `background-gdk` / `foreground-gdk` attributes | `background-rgba` / `foreground-rgba` | `inventory.c`, `spells.c` |
| `gtk_widget_modify_base` / `gtk_widget_modify_bg` | `GtkCssProvider` per widget | `stats.c`, `inventory.c` |
| `GtkStyle` / `gtk_rc_get_style_by_paths` | `GtkStyleContext` + CSS | `info.c` |
| `expose_event` signal | `draw` signal | `main.c`, `magicmap.c` |
| `gdk_colormap_alloc_color` | loop over `gdk_rgba_parse` | `main.c` |

The map renderer change (`map.c`) is architecturally significant: `gtk_map_redraw()` no longer blits to the screen directly. It stores a `cairo_surface_t` module-level and calls `gtk_widget_queue_draw`. A new `map_expose_event` draw handler does the blit with the GTK-provided `cairo_t`.

### CSS Theme System Overhaul (fork only)

The upstream's `init_theme()` added the CSS provider once at startup and never reloaded it. The fork replaces this with a stateful provider lifecycle in `config.c`:

- `apply_theme_css(path)` atomically removes the old provider and installs a new one at `GTK_STYLE_PROVIDER_PRIORITY_USER`.
- `load_theme()` now calls `apply_theme_css()` before invoking `*_get_styles()`, so the CSS is active when colors are read.
- Two exported helpers in `info.c` — `get_css_fg_color(class, out)` and `get_css_bg_color(class, out)` — allow any module to read a color from a named CSS class without coupling to `info.c` internals.

Application-specific CSS classes were added to `standard.css`:

```
.cf-stat-normal / .cf-stat-low / .cf-stat-super / .cf-stat-grad-*  → stats.c bar fill colors
.cf-inv-magical / .cf-inv-cursed / .cf-inv-unpaid                  → inventory.c row highlights
.cf-spell-attuned / .cf-spell-repelled / .cf-spell-denied          → spells.c row highlights
```

Two invalid GTK2/X11 color names that GTK3's CSS parser silently drops were fixed:
- `darkorange2` → `darkorange`
- `grey50` → `grey`

### Windows Runtime Data Path (fork only)

`CF_DATADIR` is a compile-time relative path (`./share/crossfire-client`). It is only valid when the client runs from the install root, which is never true when launched via a Windows shortcut.

The fork introduces `CF_DATADIR_RT` in `main.h`:

```c
#ifdef WIN32
extern char cf_datadir_abs[MAX_BUF];
#define CF_DATADIR_RT cf_datadir_abs
#else
#define CF_DATADIR_RT CF_DATADIR
#endif
```

`cf_datadir_abs` is populated at startup in `main.c` from the executable's location via `GetModuleFileName`. All theme path, layout path, and dialog file lookups in `config.c` and `main.c` use `CF_DATADIR_RT`.

### Inventory Tab Icon Implementation (fork only)

On Windows, `gdk_pixbuf_new_from_xpm_data()` requires the XPM loader plugin, which is absent from the bundled GTK runtime. Bundling the plugin caused a double-registration crash on startup.

The fork works around this by generating `inv_pixbufs.h` (747 lines) from the source XPM files using `gdk-pixbuf-csource`. On Windows (`#ifdef WIN32`), `inventory.c` loads tab icons from the inline byte arrays instead of calling `gdk_pixbuf_new_from_xpm_data`.

The `applied_color` constant was also updated:
```c
// Before (GdkColor literal, GTK2):
static const GdkColor applied_color = {0, 50000, 50000, 50000};

// After (GdkRGBA literal, GTK3):
static const GdkRGBA applied_color = {0.7629, 0.7629, 0.7629, 1.0};
```

---

## Refactors

### Inventory and Spell Dual Color Model (fork)

The old model stored only background colors (`inv_bg_colors[Style_Last]`). The new model tracks foreground and background separately:

```c
static GdkRGBA inv_fg_colors[Style_Last];
static GdkRGBA inv_bg_colors[Style_Last];
static bool    inv_has_fg[Style_Last];
static bool    inv_has_bg[Style_Last];
```

This allows the Black theme to apply foreground text color (for visibility on dark backgrounds) while the Standard theme applies background highlight color — both through the same code path.

### One-Shot Guards Removed from `*_get_styles()` (fork)

`inventory_get_styles()`, `spell_get_styles()`, and `stats_get_styles()` had `_init` booleans that prevented re-execution after the first call. These blocked theme switching. The guards were removed; `stats_get_styles()` now frees and reallocates `bar_colors` on every call.

### `config.h` Output Path (fork)

```cmake
# Before: lands in source tree
configure_file("config.h.in" "config.h")

# After: lands in binary directory
configure_file("${PROJECT_SOURCE_DIR}/config.h.in" "${PROJECT_BINARY_DIR}/config.h")
```

### `DEFAULT_UI` and `DIALOG_FILENAME` Moved to `main.h` (fork)

These were local `#define`s in `main.c`. The fork moves them to `main.h` as macros that reference `CF_DATADIR`, making them available to other translation units and composable with `CF_DATADIR_RT`.

### X11 Dependency Made UNIX-Only (fork)

```cmake
# Before:
find_package(X11 REQUIRED)

# After:
if(UNIX)
    find_package(X11 REQUIRED)
endif()
```

### Executable Renamed (fork)

`crossfire-client-gtk2` → `crossfire-client-gtk3` in `CMakeLists.txt` and all install rules.

---

## New Features

| Feature | Location | Notes |
|---|---|---|
| Windows application icon | `crossfire.rc`, `main.c` | `.rc` file embeds `client.ico` in the `.exe`; `main.c` sets the window icon from bundled PNGs at runtime |
| About dialog icon on Windows | `menubar.c` | Sets `GdkPixbuf` from `48x48.png` as the logo; upstream shows `image-missing` placeholder |
| `black.css` dark theme | `gtk-v2/themes/black.css` | Full dark mode: dark `textview`/`treeview` backgrounds; full set of info text and message color classes adjusted for dark backgrounds |
| CSS color lookup helpers | `info.c`: `get_css_fg_color`, `get_css_bg_color` | Exported helpers; used by `stats.c`, `inventory.c`, `spells.c` for theme-driven colors |
| GitHub Actions CI | `.github/workflows/build.yml` | Two matrix jobs: full (SDL+curl) and minimal; excludes live metaserver test; uploads binary artifact |
| Window title includes layout name | All 11 layout `.ui` files | Titles follow pattern `"Crossfire Client - GTK v3 - <Layout>"` |
| `BINDIR` and `CF_SOUND_DIR` in `config.h.in` | `config.h.in` | Two new path macros added to the generated header |
| NSIS Windows installer | `gtk-v2/win32/client.nsi` | Bundles client, DLLs, GTK runtime, data files, sounds, themes, and layouts; creates Start Menu and Desktop shortcuts |
| `AGENTS.md` | root | Contributor and AI agent guidance: source tree layout, build conventions, WIN32 coding patterns, known platform issues, pre-commit checklist |
| `BUILDING-WINDOWS.md` | root | Step-by-step Windows build and packaging guide (MSYS2/UCRT64 + NSIS) |
| `GTK3_PORT_CHANGES.md` | root | Detailed per-file developer documentation of every GTK2→GTK3 change |

---

## Removed Functionality

### Capsicum Sandbox Support (removed in fork, present in upstream)

The upstream `sf-crossfire-client-gtkv3` added FreeBSD Capsicum sandbox support (commits `16ca75a`, `073c780`). The fork removed this entirely because it was not relevant to the Windows migration goal:

- `HAVE_CAPSICUM` cmake check removed from `CMakeLists.txt`
- `#include <sys/capsicum.h>` and all `#ifdef HAVE_CAPSICUM` blocks removed from `main.c`
- `sandbox_enable` checkbox widget removed

### GIF Pixmaps Removed from `resources.xml` (fork)

The upstream `resources.xml` embeds 11 GIF files (`all.gif`, `coin.gif`, `hand.gif`, etc.) as GLib resources. The fork removes all of them. The `.gif` files themselves are absent from the fork's `pixmaps/` directory.

### `plugins/` Directory Not Present (fork)

The upstream includes `plugins/cfmaplog.py` and `plugins/cfmaplog.txt`. These are absent from the fork with no explanation in the commit log.

### Upstream Features Not Merged into Fork

The fork branched at version 1.75.3. The upstream has since reached 1.75.5 with features the fork does not have:

| Upstream commit | Feature |
|---|---|
| `db4d11d` | Config option for input debounce |
| `8badc95` | Pop-up dialog on server disconnect |
| `b184e8a` | `error_dialog()` output logged to stderr |
| `eebb9a8` | AFK monitoring only for commands actually sent |
| `4e443a9` | Static image cache removed |
| `16ca75a`, `073c780` | Capsicum sandbox support |
| `065a8c9` | `get_data_file_path()` helper function |
| `896d4d7` | Logic error fix (unspecified) |
| `026ae0b` | Mismatched definition fix |

---

## Risky Changes

### Capsicum Removal Without Replacement

The sandbox was a security boundary on FreeBSD. It was deleted rather than ported or disabled conditionally. Anyone building this fork for FreeBSD loses the security feature with no warning or fallback.

### `CF_DATADIR_RT` Relies on Silent Startup Failure

`cf_datadir_abs` is populated via `GetModuleFileName` in `main.c` on Windows. If `GetModuleFileName` fails (returns 0), `cf_datadir_abs` is left as an empty or uninitialized string and all data file lookups silently fail. The code does not log an error or abort in this case.

### `inv_pixbufs.h` Generated Header Not Tracked to Source

`inv_pixbufs.h` is a 747-line header of compiled-in pixel data generated from the source XPM files. If the XPM source files change, the header must be manually regenerated with `gdk-pixbuf-csource`. There is no build rule to do this automatically. The compiled-in data can silently drift from the source pixmaps.

### One-Shot Guards Removed — Potential Use-After-Free

`stats_get_styles()` now frees and reallocates `bar_colors` on every call. If `load_theme()` is triggered during a draw cycle that holds a pointer to the old `bar_colors` array, the draw code accesses freed memory. The original one-shot guards existed partly to prevent this re-entrancy.

### `gtk_style_context_get_background_color` Is Deprecated

`info.c` calls `gtk_style_context_get_background_color` (deprecated GTK 3.16) to read back computed CSS `background-color` values. The build suppresses the warning with `-Wno-deprecated-declarations`. This will not compile against GTK 4 and has no drop-in replacement identified.

### Theme Provider at `GTK_STYLE_PROVIDER_PRIORITY_USER`

The CSS theme is installed at `GTK_STYLE_PROVIDER_PRIORITY_USER`, the highest application-controllable priority. This overrides any system accessibility theme the user may have active. On systems with high-contrast or screen reader themes this may cause the application to render incorrectly.

### GIF Resource Removal May Break Resource Lookups

The upstream embeds GIF files as GLib resources and may have code paths that look them up by resource path. The fork removes them from `resources.xml` without auditing all consumers. Any `g_resources_lookup_data` call targeting a removed GIF path will fail at runtime.

### Version Skew From Upstream

The fork is based on version 1.75.3 and has not merged upstream's 1.75.5 bugfixes (debounce, disconnect dialog, AFK monitoring, image cache removal, miscellaneous fixes). Merging these later will require resolving conflicts against the Windows-specific changes in `config.c` and `main.c`.
