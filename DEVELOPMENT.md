# Development Comparison

Comparison of:
- **Upstream**: <https://sourceforge.net/p/crossfire/crossfire-client/ci/gtk3/tree/> - SourceForge GTK3 port branch, version 1.75.5
- **Fork**: <https://github.com/tannerrj/windows-client-gtkv3-experiment-sf> - Windows packaging fork, version 1.75.3, branched from upstream at commit `4285a62`

The fork's goal is a self-contained Windows installer built from the GTK3 port while keeping all changes non-breaking on Linux/macOS.

---

## Major Code Changes

### GTK2 -> GTK3 API Migration (both repos, fork has more coverage)

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
- Two exported helpers in `info.c` - `get_css_fg_color(class, out)` and `get_css_bg_color(class, out)` - allow any module to read a color from a named CSS class without coupling to `info.c` internals.
- `read_config_dialog()` passes the file chooser result through `g_canonicalize_filename()` before storing it, guaranteeing `client.ini` always records an absolute path. The same applies to the UI layout file chooser. A pre-existing memory leak (when the chosen theme equalled the current theme) was also fixed.
- `setup_config_dialog()` uses `gtk_file_chooser_set_current_folder()` on Windows (the native file dialog silently ignores `set_filename()`) and `gtk_file_chooser_set_filename()` on other platforms, controlled via `#ifdef _WIN32` / `#else` blocks.

Application-specific CSS classes were added to `standard.css`:

```
.cf-stat-normal / .cf-stat-low / .cf-stat-super / .cf-stat-grad-*  -> stats.c bar fill colors
.cf-inv-magical / .cf-inv-cursed / .cf-inv-unpaid                  -> inventory.c row highlights
.cf-spell-attuned / .cf-spell-repelled / .cf-spell-denied          -> spells.c row highlights
```

Two invalid GTK2/X11 color names that GTK3's CSS parser silently drops were fixed:
- `darkorange2` -> `darkorange`
- `grey50` -> `grey`

### Windows Runtime Data Path (fork only)

`CF_DATADIR` is a compile-time relative path (`./share/crossfire-client`). It is only valid when the client runs from the install root, which is never true when launched via a Windows shortcut.

The fork introduces `CF_DATADIR_RT` in `main.h`:

```c
#ifdef _WIN32
extern char cf_datadir_abs[MAX_BUF];
#define CF_DATADIR_RT cf_datadir_abs
#else
#define CF_DATADIR_RT CF_DATADIR
#endif
```

`cf_datadir_abs` is populated at startup in `main.c` from the executable's location via `GetModuleFileName`. All theme path, layout path, and dialog file lookups in `config.c` and `main.c` use `CF_DATADIR_RT`.

### Inventory Tab Icon Implementation (fork only)

On Windows, `gdk_pixbuf_new_from_xpm_data()` requires the XPM loader plugin, which is absent from the bundled GTK runtime. Bundling the plugin caused a double-registration crash on startup.

The fork works around this by generating `inv_pixbufs.h` (747 lines) from the source XPM files using `gdk-pixbuf-csource`. On Windows (`#ifdef _WIN32`), `inventory.c` loads tab icons from the inline byte arrays instead of calling `gdk_pixbuf_new_from_xpm_data`.

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

This allows the Black theme to apply foreground text color (for visibility on dark backgrounds) while the Standard theme applies background highlight color - both through the same code path.

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

`crossfire-client-gtk2` -> `crossfire-client-gtk3` in `CMakeLists.txt` and all install rules.

---

## Performance Optimisations (fork)

These changes were made on the `gtk3-client-performance-improvements` branch. Changes span `gtk-v2/src/map.c`, `gtk-v2/src/inventory.c`, `common/mapdata.c`, `gtk-v2/src/main.c`, `gtk-v2/src/image.c`, and `common/client.c`.

### Map Renderer

#### Dirty-Region Tracking in `gtk_map_redraw()`

The original renderer ran a full tile redraw every call regardless of whether any tile data had changed. The fork adds per-frame dirty-cell scanning (`need_update` / `need_resmooth` flags on `MapCell`) and skips the tile-render phase entirely when the viewport is stable and no cells are dirty. Only the compose phase runs for pure animation frames (sub-tile smooth-scroll offset draining to zero).

#### `display_mapscroll` Blit Optimisation

`display_mapscroll(dx, dy)` previously always returned 0, forcing a full tile redraw on every map scroll. The fork implements the blit path:

- A persistent `tile_surface` (ARGB32) holds the tile+label layer. On scroll, its contents are shifted by `dx * map_image_size` pixels in place.
- `global_offset_x += pixel_dx` keeps the composition phase visually aligned (cancels the blit shift until `want_offset_x` drains).
- `want_offset_x -= dx` removes the consumed prediction so smooth-scroll animation settles correctly.
- Only the newly exposed strip at the scroll edge is re-rendered; the rest of the tile surface is reused.
- Pixel-interpolated lighting modes (`CFG_LT_PIXEL`, `CFG_LT_PIXEL_BEST`) and diagonal scrolls (`dx != 0 && dy != 0`) fall back to 0 (full redraw) as partial updates are incompatible with those modes.

#### Darkness Overlay - Cached Surface and Direct Pixel Writes

`draw_darkness()` previously allocated a new `cairo_surface_t` on every frame and painted each darkness cell with a `cairo_rectangle` / `cairo_fill` pair. The fork replaces this with:

- A module-level `lm_surface` (ARGB32) reallocated only when the tile-count (`nx`, `ny`) or lighting mode changes.
- Direct buffer writes via `cairo_image_surface_get_data()` with a single `(uint32_t)alpha << 24` per cell, eliminating all per-pixel Cairo draw calls.

#### Software-Renderer Overhead Reduction (RGB24 + OPERATOR_SOURCE)

The GDK Win32 backend has no GPU acceleration - all Cairo rendering uses a software rasteriser. Three changes reduce unnecessary per-pixel arithmetic:

| Location | Change | Reason |
|---|---|---|
| `map_surface` allocation | `CAIRO_FORMAT_ARGB32` -> `CAIRO_FORMAT_RGB24` | Composed output is always opaque; RGB24 skips alpha premultiplication in the final blit |
| Phase-2 black fill in `gtk_map_redraw()` | `CAIRO_OPERATOR_SOURCE` instead of default `OVER` | Equivalent result for solid opaque fill; avoids alpha-blend arithmetic |
| `map_expose_event()` screen blit | `CAIRO_OPERATOR_SOURCE` instead of default `OVER` | `map_surface` is RGB24 (opaque); SOURCE is correct and faster |

`OVER` is restored before blitting `tile_surface` so transparent sprite pixels blend correctly. These changes benefit all platforms; no WIN32 guards are needed.

**Note:** True GPU acceleration would require porting the map renderer to `GtkGLArea` / OpenGL. That is a large architectural change not yet attempted.

#### Darkness Upscale - Manual Bilinear Replaces `CAIRO_FILTER_GOOD/BEST`

Pixel lighting mode (`CFG_LT_PIXEL`, `CFG_LT_PIXEL_BEST`) previously stored a tile-resolution light map in `lm_surface` and upscaled it to full pixel resolution using `CAIRO_FILTER_GOOD` or `CAIRO_FILTER_BEST`. Those filters invoke Cairo's internal Lanczos/bilinear compositor, which has significant per-pixel overhead on the software rasteriser.

The fork pre-expands the light map to full pixel resolution using manual bilinear interpolation before writing to `lm_surface`. The resulting surface is blitted 1:1 with `CAIRO_FILTER_NEAREST` (no upscaling needed). The inner bilinear loop uses integer `fx`/`fy` counters and a single integer multiply-accumulate, with no floating-point division.

Tile lighting mode is unaffected - it still uses one pixel per tile with `CAIRO_FILTER_NEAREST`.

#### Smooth-Tile Inner Loop - Direct Pixel OVER Blend

`draw_smooth_pixmap()` is called once per smooth sub-tile during the layer rendering pass. Previously it used `cairo_set_source_surface` + `cairo_rectangle` + `cairo_fill` to copy each sub-tile into `tile_surface`, invoking the full Cairo compositor for every call.

In full-redraw mode (non-scroll frames), `tile_surface`'s raw pixel buffer is exposed via `cairo_image_surface_get_data()` before the layer loop begins. `draw_smooth_pixmap()` blends directly into that buffer using a two-channel premultiplied ARGB32 OVER formula:

```c
const uint32_t inv = 256 - sa;
const uint32_t rb  = (sp & 0x00FF00FF)
                   + (((dp & 0x00FF00FF) * inv) >> 8 & 0x00FF00FF);
const uint32_t ag  = ((sp >> 8) & 0x00FF00FF)
                   + ((((dp >> 8) & 0x00FF00FF) * inv) >> 8 & 0x00FF00FF);
*drow++ = (ag << 8) | rb;
```

This processes two channels in parallel with one mask and shift each, avoiding per-pixel Cairo compositor overhead. The Cairo path is preserved as a fallback for partial-update (scroll blit) frames, where Cairo's clip region must be respected.

### Inventory Renderer

#### Differential Update - No Full Rebuild per Change

`draw_look_list()` and `draw_inv_list()` previously called `gtk_tree_store_clear()` then rebuilt the entire `GtkTreeStore` from scratch on every server update. For large inventories this is O(n) GtkTreeStore operations per update even when only one item changed.

The fork adds `try_diff_update_look()` and `try_diff_update_inv()` helpers that walk the store and the item list in parallel:

- If the item at a given position matches the store row, the row is updated in-place via `gtk_tree_store_set()`.
- New items at the end are appended.
- Stale rows at the end are removed with `gtk_tree_store_remove()`.
- If the item order has changed (different `item*` at the same position), the helper returns `FALSE` and the caller falls back to a full rebuild.

Full rebuilds are also forced when a container is open (child rows would need hierarchical handling). In typical play only the last few items change per tick, so the differential path avoids most store operations.

#### Icon-View Event Mask - `GDK_ALL_EVENTS_MASK` Removed

`draw_inv_table()` previously called `gtk_widget_add_events(cell, GDK_ALL_EVENTS_MASK)` on every cell on every redraw cycle, regardless of whether the cell was newly created. `GDK_ALL_EVENTS_MASK` subscribes to every input event (pointer motion, scroll, crossing, key, focus, ...) and is far broader than needed.

The `add_events` call is now in the one-time cell-creation block and uses only `GDK_BUTTON_PRESS_MASK`. Tooltip-related masks (`GDK_POINTER_MOTION_MASK`, `GDK_LEAVE_NOTIFY_MASK`) are added automatically by GTK when `gtk_widget_set_tooltip_text` sets the `has-tooltip` property, so they do not need to be specified manually.

#### Shared `GtkCssProvider` for Applied-Item Highlight

`draw_inv_table()` previously called `gtk_css_provider_new()`, `gtk_css_provider_load_from_data()`, and `g_object_unref()` on every cell during every redraw to apply the grey background for applied items. With a full inventory redraw at every tick this allocated and freed a provider object per visible cell.

A module-level `static GtkCssProvider *applied_css_provider` is now initialized once on first use. Per-cell state (`was_applied`) is tracked via `g_object_set_data(G_OBJECT(cell), "inv-applied", ...)` using `GINT_TO_POINTER(1)` as a boolean sentinel. The provider is added or removed from the cell's style context only when the applied state actually changes, skipping the add/remove entirely for the common case.

### Main Loop and Animation

#### Redraw Idle Guard - `g_idle_add` Accumulation Prevented

`self_tick()` (the 8 Hz animation timer) previously called `g_idle_add(redraw, NULL)` unconditionally every tick. If GTK was busy and the previous redraw idle had not yet run, a second (and third, ...) `redraw` callback was queued on top of it, causing multiple `draw_map()` + `draw_lists()` calls per frame.

A `static guint redraw_idle_id` is set by `g_idle_add` and cleared to 0 at the start of the `redraw` callback. `self_tick()` only calls `g_idle_add` when `redraw_idle_id == 0`, guaranteeing at most one pending redraw in the idle queue.

#### `mapdata_animation()` - Bounded SYNC Animation Scan

`mapdata_animation()` iterated all 2000 `MAXANIM` slots in the `animations[]` array every tick to advance synchronized animation phases, even though most slots are empty (no SYNC speed assigned). In practice only a small fraction of slots are used by any given server.

A `static int anim_sync_max` high-water mark (one past the highest `animations[]` index that has ever received a SYNC speed in `mapdata_set_anim_layer`) replaces `MAXANIM` as the loop bound. The scan is proportional to the number of distinct synchronized animation IDs the server has sent, which is typically far smaller than 2000.

### Startup and Connection

#### `image_update_download_status` - Spin-Loop Replaced

`image_update_download_status()` drove the image-download progress bar with `while(gtk_events_pending()) { gtk_main_iteration(); }` - draining the entire GTK event queue on every progress update. If animation timers or incoming network data kept producing events during the download, this loop would spin indefinitely, stalling the download for each call.

Replaced with a single `g_main_context_iteration(NULL, FALSE)`, which dispatches at most one pending event per call (sufficient to process the queued progress-bar repaint) and returns immediately whether or not any events were pending.

#### TCP_NODELAY Enabled on Windows

The `CONFIG_FASTTCP` ("Fast TCP") preference was blocked by `#ifndef WIN32` guards in both `common/client.c` and `gtk-v2/src/config.c`, so the setting had no effect on Windows. Windows Winsock supports `TCP_NODELAY` via `setsockopt` with `IPPROTO_TCP` as the level and `(const char*)` as the optval type.

`client.c` now uses `#if defined(HAVE_GIO_GNETWORKING_H)` / `#elif defined(_WIN32)` to dispatch to the correct ABI. `config.c` received the same split and also fixed a pre-existing bug where `csocket.fd` (a `GSocketConnection*`) was passed directly to `setsockopt` instead of extracting the raw fd via `g_socket_connection_get_socket()` + `g_socket_get_fd()`. A missing `#include <gio/gnetworking.h>` (needed for `TCP_NODELAY` on POSIX) was also added to `config.c`.

#### `my_log_handler` - 1-Second Sleep Removed

`my_log_handler` is a debugging aid (a GLib log handler meant to be set as a breakpoint target when chasing GTK assertion failures). Its body contained `g_usleep(1 * 1e6)` - a 1-second freeze - which would fire for every GTK log message if the handler were ever registered via `g_log_set_handler`. The sleep is removed; the function body is now a no-op so it remains a valid breakpoint target.

---

## Bug Fixes

### Spurious "Unable to find match for faceset" on First Launch (`common/image.c`)

When no faceset preference has been saved, `load_config()` in `config.c` sets
`face_info.want_faceset` to `""` (empty string) as a "no preference" sentinel.
The faceset name-matching block in `image.c` tested only
`want_faceset != NULL && atoi(want_faceset) == 0`, which is satisfied by `""`:
the pointer is non-NULL and `atoi("") == 0`. The loop searched every faceset
slot for a match with an empty string, found none, and printed a red
`MSG_TYPE_CLIENT_CONFIG` message:

```
Unable to find match for faceset  on the server
```

(Note the double space - the empty `want_faceset` value is interpolated
directly into the format string.)

Fix: added `face_info.want_faceset[0] != '\0'` to the guard. An empty string
is now treated identically to NULL - the block is skipped and the server's
default faceset is used, which is the correct behaviour when the user has
expressed no preference.

### Spurious "Message Control settings not loaded" on First Launch (`gtk-v2/src/info.c`)

`msgctrl_init()` calls `load_msgctrl_configuration()` at startup to restore
saved Message Control settings from `config_dir/msgs`. On a fresh install the
file has never been created; `fopen` returns NULL with `errno == ENOENT`. The
original code treated this identically to any other `fopen` failure - a
permissions error, a disk error - and printed a red `MSG_TYPE_CLIENT_ERROR`
message naming the full file path.

Fix: after `fopen` fails, `errno` is checked. If `errno == ENOENT` the
function returns silently, leaving the defaults set by the preceding
`default_msgctrl_configuration()` call in place. Any other `errno` value
(e.g. `EACCES`) still shows the red error, since that indicates a real problem.
`#include <errno.h>` was added to `info.c`.

---

## New Features

| Feature | Location | Notes |
|---|---|---|
| Windows application icon | `crossfire.rc`, `main.c` | `.rc` file embeds `client.ico` in the `.exe`; `main.c` sets the window icon from bundled PNGs at runtime |
| About dialog icon on Windows | `menubar.c` | Sets `GdkPixbuf` from `48x48.png` as the logo; upstream shows `image-missing` placeholder |
| `black.css` dark theme | `gtk-v2/themes/black.css` | Full dark mode: dark `textview`/`treeview` backgrounds; full set of info text and message color classes adjusted for dark backgrounds |
| CSS color lookup helpers | `info.c`: `get_css_fg_color`, `get_css_bg_color` | Exported helpers; used by `stats.c`, `inventory.c`, `spells.c` for theme-driven colors |
| GitHub Actions CI | `.github/workflows/build.yml` | Three jobs: Linux full (SDL+curl), Linux minimal (no optional deps), Windows (MSYS2/UCRT64); excludes live metaserver test; Linux full and Windows jobs upload binary artifacts; actions updated to Node 24 runtime (`checkout@v6`, `upload-artifact@v7`) |
| Window title includes layout name | All 11 layout `.ui` files | Titles follow pattern `"Crossfire Client - GTK v3 - <Layout>"` |
| `BINDIR` and `CF_SOUND_DIR` in `config.h.in` | `config.h.in` | Two new path macros added to the generated header |
| NSIS Windows installer | `gtk-v2/win32/client.nsi` | Bundles client, DLLs, GTK runtime, data files, sounds, themes, and layouts; creates Start Menu and Desktop shortcuts |
| `AGENTS.md` | root | Contributor and AI agent guidance: source tree layout, build conventions, WIN32 coding patterns, known platform issues, pre-commit checklist |
| `BUILDING-WINDOWS.md` | root | Step-by-step Windows build and packaging guide (MSYS2/UCRT64 + NSIS) |
| `GTK3_PORT_CHANGES.md` | root | Detailed per-file developer documentation of every GTK2->GTK3 change |
| Capsicum sandbox (FreeBSD) | `CMakeLists.txt`, `config.h.in`, `main.c`, `map.c`, `dialogs.ui` | Restored from upstream commits `16ca75a`, `1e5e75d`, `073c780`; gated on `HAVE_CAPSICUM` so Linux/macOS/Windows builds are unaffected; checkbox greyed out on non-FreeBSD platforms |

---

## Removed Functionality

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
| `065a8c9` | `get_data_file_path()` helper function |
| `896d4d7` | Logic error fix (unspecified) |
| `026ae0b` | Mismatched definition fix |

Upstream commits `16ca75a`, `1e5e75d`, and `073c780` (Capsicum sandbox support) have been merged into this fork.

---

## Risky Changes

### `CF_DATADIR_RT` Relies on Silent Startup Failure

`cf_datadir_abs` is populated via `GetModuleFileName` in `main.c` on Windows. If `GetModuleFileName` fails (returns 0), `cf_datadir_abs` is left as an empty or uninitialized string and all data file lookups silently fail. The code does not log an error or abort in this case.

### `inv_pixbufs.h` Generated Header Not Tracked to Source

`inv_pixbufs.h` is a 747-line header of compiled-in pixel data generated from the source XPM files. If the XPM source files change, the header must be manually regenerated with `gdk-pixbuf-csource`. There is no build rule to do this automatically. The compiled-in data can silently drift from the source pixmaps.

### One-Shot Guards Removed - Potential Use-After-Free

`stats_get_styles()` now frees and reallocates `bar_colors` on every call. If `load_theme()` is triggered during a draw cycle that holds a pointer to the old `bar_colors` array, the draw code accesses freed memory. The original one-shot guards existed partly to prevent this re-entrancy.

### `gtk_style_context_get_background_color` Is Deprecated

`info.c` calls `gtk_style_context_get_background_color` (deprecated GTK 3.16) to read back computed CSS `background-color` values. The build suppresses the warning with `-Wno-deprecated-declarations`. This will not compile against GTK 4 and has no drop-in replacement identified.

### Theme Provider at `GTK_STYLE_PROVIDER_PRIORITY_USER`

The CSS theme is installed at `GTK_STYLE_PROVIDER_PRIORITY_USER`, the highest application-controllable priority. This overrides any system accessibility theme the user may have active. On systems with high-contrast or screen reader themes this may cause the application to render incorrectly.

### GIF Resource Removal May Break Resource Lookups

The upstream embeds GIF files as GLib resources and may have code paths that look them up by resource path. The fork removes them from `resources.xml` without auditing all consumers. Any `g_resources_lookup_data` call targeting a removed GIF path will fail at runtime.

### Version Skew From Upstream

The fork is based on version 1.75.3 and has not merged upstream's 1.75.5 bugfixes (debounce, disconnect dialog, AFK monitoring, image cache removal, miscellaneous fixes). Merging these later will require resolving conflicts against the Windows-specific changes in `config.c` and `main.c`.
