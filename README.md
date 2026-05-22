# Crossfire Client — GTK3 Windows Port

Crossfire is a free, open-source, cooperative multi-player RPG and adventure
game. Since its initial release, Crossfire has grown to encompass over 150
monsters, 3000 areas to explore, an elaborate magic system, 13 races, 15
character classes, and many powerful artifacts scattered far and wide. Set in a
fantastical medieval world, it blends the style of Gauntlet, NetHack, Moria,
and Angband.

- **Website:** http://crossfire.real-time.com/
- **Wiki:** http://wiki.cross-fire.org/

---

## About this repository

This repository is a fork of the upstream Crossfire GTK3 client port, which
originates from SourceForge at:

> https://sourceforge.net/p/crossfire/crossfire-client/ci/gtk3/tree/

The fork point is commit
`4285a62cd3de64190957144d505f636dcd3d6926` — the tip of the upstream `gtk3`
branch at the time this work began. All commits beyond that point are original
to this repository.

The goal of this fork is a fully working, self-contained Windows installer
built from the GTK3 port, while keeping all changes non-breaking on Linux and
macOS.

All new code in this repository was developed with the assistance of
**Claude Sonnet 4.6** (Anthropic).

### Why the long name?

The code repo name is `windows-client-gtkv3-experiment-sf`

To show what the code is looking to build, the frame work version, that this is all an experiment,
and the origin of the code base from SourceForge.

---

## Why GTK3? Moving away from GTK2

### For players

If you have played Crossfire before and are wondering why the client looks
slightly different, or why a new installer is needed: the short answer is that
the old graphical toolkit the client was built on — GTK2 — is no longer
maintained and is quietly disappearing from modern operating systems. GTK2 was
released in 2002 and its final version shipped in 2011. Linux distributions are
dropping it, and on Windows it was never easy to install in the first place.
The move to GTK3 means the Crossfire client can continue to run on current
systems without depending on aging, unsupported libraries. For Windows players
in particular, this fork packages everything the client needs into a single
installer, so there is nothing extra to set up.

### For developers

GTK2 reached end-of-life with version 2.24 (2011) and has received no upstream
development since. GTK3, first released in 2011, completed the transition to a
modern rendering pipeline and has been the supported stable series ever since
(with GTK 3.24 as the current long-term release). Several GTK2 APIs were
deprecated across GTK 3.0–3.22 and formally removed in GTK 4; compiling the
old client against a current GTK3 installation produces hundreds of deprecation
warnings and in some cases link errors. The port replaces the most impactful
deprecated APIs — `GdkColor`, `GtkTable`, `GtkHSeparator`, `GTK_STOCK_*` items,
`gdk_cairo_create`, `gtk_widget_modify_base/bg`, and the RC-file theme system —
with their GTK3 equivalents (`GdkRGBA`, `GtkGrid`, `GtkSeparator`, mnemonic
labels, the `draw` signal with a persistent `cairo_surface_t`, `GtkCssProvider`,
and CSS-based theming). The result compiles cleanly against GTK 3.24 with
`-Wno-deprecated-declarations`, and passes a strict build with
`-DGTK_DISABLE_DEPRECATED` for the subset of APIs that have clean replacements.
A full description of every changed API and the rationale behind each decision
is in `GTK3_PORT_CHANGES.md`.

---

## What was fixed

The upstream GTK3 port compiled and ran on Linux but had several issues that
prevented it from being usable as a Windows installer. The following problems
were identified and resolved. For full technical details, refer to the git
commit log.

- **CMake Unix-only targets on Windows** — X11 detection and Linux desktop
  icon install rules failed the Windows build.

- **Relative data paths at runtime** — The compile-time `CF_DATADIR` path is
  relative and only valid if the client is launched from the install root. All
  runtime data lookups (themes, layouts, icons) silently failed when the client
  was started from a shortcut.

- **Layout file chooser** — The Preferences → Layout file chooser opened to
  an undefined location rather than the installed UI directory.

- **CSS theme loading** — The GTK3 CSS theme failed to load entirely on
  Windows, causing all message text in the info pane to render in a single
  default color with no per-message-type colorization.

- **Theme file chooser** — The Preferences → Theme file chooser opened to an
  undefined location rather than the installed themes directory. GTK2-format
  theme files (`Standard`, `Black`) appeared alongside the valid CSS files and
  would fail silently if selected.

- **About dialog icon** — The Help → About dialog displayed a generic
  placeholder icon (`image-missing`) instead of the Crossfire application icon.

- **Inventory filter tab icons** — The inventory panel filter tabs (All,
  Applied, Unapplied, Unpaid, Cursed, Magical, etc.) displayed no icons. The
  XPM loader plugin required by `gdk_pixbuf_new_from_xpm_data()` is not
  present in the bundled GTK runtime, and bundling it caused a double-
  registration crash.

---

## What was added

- **Windows application icon** — The Crossfire icon is embedded in the
  compiled `.exe` via a Windows resource file, and the GTK window icon is set
  at runtime from the bundled PNG files. The installer and desktop/Start Menu
  shortcuts also use the Crossfire icon.

- **Layout name in the title bar** — The main window title bar displays the
  name of the currently active UI layout (e.g. `Crossfire Client GTK v3 —
  sixforty`), making it easy to identify which layout is in use. This feature
  was present in the upstream commits before the fork point.

- **Windows installer** — A self-contained NSIS `.exe` installer bundles the
  client, all required DLLs, the GTK runtime, data files, sounds, themes, and
  layout files. The installer creates Start Menu and Desktop shortcuts with the
  correct working directory and application icon.

- **AGENTS.md** — Contributor and AI agent guidance document covering the
  source tree layout, build conventions, WIN32 coding patterns, known platform
  issues, and a pre-commit testing checklist.

- **BUILDING-WINDOWS.md** — Full step-by-step documentation for building and
  packaging the Windows installer from source using MSYS2/UCRT64.

---

## Performance improvements

The `gtk3-client-performance-improvements` branch adds a series of rendering and
loop optimisations that reduce CPU usage during normal play. For full technical
details see `DEVELOPMENT.md`.

**Map renderer**

- Dirty-region tracking skips the tile-render phase entirely on stable frames
- `display_mapscroll` blit reuses the tile surface on scroll instead of full redraws
- Darkness overlay uses a cached surface and direct pixel writes instead of Cairo `fill` calls
- RGB24 map surface and `OPERATOR_SOURCE` operator reduce software-rasteriser overhead
- Pixel lighting mode pre-expands the light map with manual bilinear interpolation (avoids `CAIRO_FILTER_GOOD/BEST` upscale overhead)
- Smooth sub-tile inner loop blends directly into the pixel buffer (avoids per-sub-tile Cairo compositor calls)

**Inventory renderer**

- Differential store update replaces full `GtkTreeStore` rebuild on every server tick
- `GDK_BUTTON_PRESS_MASK` replaces `GDK_ALL_EVENTS_MASK` on icon-view cells
- Shared `GtkCssProvider` for applied-item highlight (was allocated and freed per cell per frame)

**Main loop and startup**

- Redraw idle guard prevents `g_idle_add` accumulation when GTK is busy
- `my_log_handler` 1-second blocking sleep removed
- `image_update_download_status` spin-loop replaced with a single `g_main_context_iteration` call
- `mapdata_animation` SYNC scan bounded by a high-water mark instead of the full 2000-slot array
- TCP_NODELAY (`CONFIG_FASTTCP`) enabled on Windows via the Winsock2 path

---

## Platform compatibility

All Windows-specific changes are guarded by `#ifdef _WIN32` (C source) or
`if(WIN32)` (CMake) conditions. The Linux and macOS build paths are unchanged. The GitHub Actions CI
workflow builds the client in three jobs on every push or pull request to the
`gtk3` and `master` branches — two Linux configurations and one Windows build —
and confirms that none of the platform-specific changes break any other target.
See `.github/workflows/build.yml`.

The CI workflow uses Ninja as the build system and explicitly disables Lua
(`-DLUA=OFF`), matching the Windows installer build configuration. The full
Linux build and the Windows build each upload the compiled binary as a workflow
artifact (`crossfire-client-gtk3` and `crossfire-client-gtk3.exe` respectively).
The Windows job uses MSYS2/UCRT64 (`windows-latest` runner, `msys2/setup-msys2`
action) to match the documented developer build environment.

---

## Building

### Linux / macOS

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Windows installer

See **[BUILDING-WINDOWS.md](BUILDING-WINDOWS.md)** for the full Windows build
and packaging workflow using MSYS2/UCRT64 and NSIS.

---

## License and compliance

This project is licensed under the **GNU General Public License version 2**
(GPLv2), the same license as the upstream Crossfire client. See
[COPYING](COPYING) for the full license text.

All code changes introduced in this fork are GPLv2-compliant:

- The Windows resource file (`crossfire.rc`) embeds the existing `client.ico`
  which is part of the upstream source tree.
- The inline pixbuf header (`inv_pixbufs.h`) is generated from the existing
  XPM icon files in `pixmaps/` using `gdk-pixbuf-csource`, a standard GLib
  tool. The generated data is a byte-for-byte representation of those icons and
  carries the same license.
- All other new code consists of C source additions and CMake/build system
  changes that are original work and are contributed under GPLv2.
- No third-party code with an incompatible license has been introduced.

The NSIS installer script produces a GPLv2-compliant distributable: it bundles
only the client executable, the GTK runtime libraries (LGPL), and the
Crossfire data files (GPLv2).

---

## Upstream

The upstream GTK3 port and the broader Crossfire client history are maintained
at SourceForge. This fork tracks the `gtk3` branch. Patches that are
appropriate for upstream should be submitted there.

- SourceForge project: https://sourceforge.net/p/crossfire/crossfire-client/
- Upstream GTK3 branch: https://sourceforge.net/p/crossfire/crossfire-client/ci/gtk3/tree/