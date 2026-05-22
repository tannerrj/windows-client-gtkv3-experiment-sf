# AGENTS.md — Crossfire GTK3 Client

This file describes the repository structure, build system, conventions, and
guidelines for AI agents (and human contributors) working on this codebase.

---

## Repository overview

This is the **GTK3 port** of the Crossfire client, maintained on the `gtk3`
branch. The upstream GTK2 client lives on `master`. The goal of this branch is
a working GTK3 build that can be packaged as a self-contained Windows installer
while remaining buildable on Linux and macOS.

```
.
├── common/                  Shared client library (protocol, init, state)
│   ├── client.c             Global variables including config_dir, cache_dir
│   ├── client.h             Externs for shared globals
│   └── init.c               Sets config_dir via g_get_user_config_dir()
├── gtk-v2/
│   ├── src/                 GTK3 client source
│   │   ├── main.c           Entry point, window setup, WIN32 startup
│   │   ├── main.h           CF_DATADIR_RT macro, cf_datadir_abs extern
│   │   ├── config.c         Preferences, theme loading, config file I/O
│   │   ├── config.h
│   │   ├── info.c           Info/message pane
│   │   ├── keys.c           Keybinding support
│   │   ├── map.c            Map rendering
│   │   ├── image.c          Image/face handling
│   │   ├── inventory.c      Inventory panel
│   │   ├── stats.c          Character stats panel
│   │   ├── metaserver.c     Server browser
│   │   ├── account.c        Account login/creation
│   │   ├── create_char.c    Character creation
│   │   ├── crossfire.rc     Windows resource file (embeds client.ico)
│   │   └── CMakeLists.txt   Per-target build rules
│   ├── ui/                  GtkBuilder UI definition files
│   │   ├── dialogs.ui       Preferences, keybinding, about, spell dialogs
│   │   └── *.ui             Layout files (sixforty.ui, divided_112.ui, etc.)
│   └── CMakeLists.txt       GTK-v2 subdirectory cmake
├── pixmaps/                 Application icons
│   ├── client.ico           Windows icon (9 sizes, 16-bit colour)
│   ├── 16x16.png
│   ├── 32x32.png
│   └── 48x48.png
├── sounds/                  Sound effects
├── CMakeLists.txt           Root cmake
├── BUILDING-WINDOWS.md      Windows installer build guide
└── AGENTS.md                This file
```

---

## Build system

CMake with the Ninja generator. The canonical build sequence is:

```bash
cd ~/client/windows-client-gtkv3-experiment-sf
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The output executable is `build/gtk-v2/src/crossfire-client-gtk3.exe` on
Windows or `build/gtk-v2/src/crossfire-client-gtk3` on Linux/macOS.

On Windows, build inside the **MSYS2 UCRT64** shell only. Do not use CMD or
PowerShell for compilation.

See `BUILDING-WINDOWS.md` for the full Windows packaging workflow including
staging, DLL bundling, and NSIS packaging.

---

## Key conventions

### File paths in Python scripts

When editing source files from Python in the MSYS2 environment, always use the
full Windows-style path:

```python
# Correct
open('C:/msys64/home/leaf/client/windows-client-gtkv3-experiment-sf/gtk-v2/src/config.c')

# Wrong — Python resolves ~ to C:\Users\leaf, not the MSYS2 home
open('~/client/...')
```

Always open `.ui` files with `encoding='utf-8'` — they contain non-ASCII
characters and will fail with the default Windows cp1252 codec.

### WIN32 guards

All Windows-specific code must be wrapped in `#ifdef WIN32` / `#endif`. The
`#else` branch should contain the original cross-platform code. Never leave an
empty `#else` block without a comment.

```c
#ifdef WIN32
    /* Windows-specific implementation */
    ...
#else
    /* Original cross-platform code */
    ...
#endif
```

### Cross-platform `setsockopt`

POSIX networking constants (`TCP_NODELAY`, `SOL_TCP`, `IPPROTO_TCP`) come from
`<gio/gnetworking.h>`, which is not available on Windows. On Windows they are
defined in `<winsock2.h>` (already included via `client.h`). Use the
`HAVE_GIO_GNETWORKING_H` / `WIN32` split rather than a single `#ifndef WIN32`:

```c
#ifdef HAVE_GIO_GNETWORKING_H
#include <gio/gnetworking.h>
#endif

// At the call site:
#if defined(HAVE_GIO_GNETWORKING_H)
    if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &i, sizeof(i)) == -1)
        perror("TCP_NODELAY");
#elif defined(WIN32)
    /* Winsock setsockopt requires (const char*) for optval. */
    setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&i, sizeof(i));
#endif
```

To get the raw socket fd from a `GSocketConnection*`, use
`g_socket_connection_get_socket()` + `g_socket_get_fd()`.  Never pass a
`GSocketConnection*` directly to `setsockopt`.

### GTK event loop re-entrancy

Never drain the GTK event queue with `while(gtk_events_pending()) { gtk_main_iteration(); }` from within network-processing code or any callback that runs
outside the main GTK loop.  If the queue never empties (animation timers,
network events) this loop spins indefinitely.  Use a single
`g_main_context_iteration(NULL, FALSE)` to process at most one pending event
and return immediately.

### CF_DATADIR_RT

`CF_DATADIR` is a compile-time constant that expands to a relative path
(`./share/crossfire-client`). On Windows this is invalid at runtime when the
client is launched from a shortcut. Always use `CF_DATADIR_RT` instead, which
expands to the runtime absolute data path on Windows and `CF_DATADIR` on other
platforms. `CF_DATADIR_RT` is defined in `main.h` and populated at startup in
`main.c`.

### GtkFileChooser on Windows

The Windows native file dialog ignores `GtkFileFilter` rules entirely. Do not
rely on GTK file filters to restrict what the user sees. Instead, ensure only
valid files exist in the relevant directories (for example, do not install
GTK2-format theme files alongside `.css` files).

On Windows the native file dialog silently ignores
`gtk_file_chooser_set_filename()`. Always use
`gtk_file_chooser_set_current_folder()` with an absolute path built from
`CF_DATADIR_RT` inside a `#ifdef WIN32` block, and call
`gtk_file_chooser_set_filename()` only in the `#else` branch for non-Windows
platforms. Do not attempt to probe with `get_filename()` as a fallback — the
call will return NULL on Windows because `set_filename` was silently ignored,
not because the path is stale.

When reading a path back from a file chooser in `read_config_dialog()`, always
pass the result through `g_canonicalize_filename(buf, NULL)` before storing it.
This guarantees that only absolute paths are written to `client.ini`, regardless
of what the chooser widget returns.

### UI files

`dialogs.ui` and the layout `.ui` files are GtkBuilder XML. They must remain
valid GtkBuilder XML at all times — the client will display an error dialog and
exit if `gtk_builder_add_from_file()` fails. Always test UI file changes by
copying them to the install directory and launching the client before
committing. Never embed a `GtkFileFilter` as a sibling object inside a
`<child>` block; define it as a top-level object inside `<interface>` if
needed.

### Themes

The themes directory (`share/crossfire-client/themes/`) must contain only
`.css` files on Windows. The GTK2-format `Standard` and `Black` files are
incompatible with GTK3's CSS provider and must not be installed.

---

## Commit message style

Follow the pattern established in this branch:

```
subsystem[, subsystem]: Short imperative summary (≤72 chars)

Longer explanation of what was broken and why. Include:
- The symptom the user experienced
- The root cause
- What the fix does
- Any relevant caveats or platform restrictions

WIN32-only changes end with:
"All changes are guarded by WIN32 conditions and do not affect
Linux or macOS builds."
```

Examples of subsystem prefixes used in this branch: `cmake`, `config`, `main`,
`docs`.

---

## Config file

The client stores user settings at:

```
Windows:  C:\Users\<user>\AppData\Local\crossfire\client.ini
Linux:    ~/.config/crossfire/client.ini
```

The `[GTKv2]` section stores `theme` and `window_layout` as absolute paths on
Windows. If these paths become stale (for example after reinstalling to a
different directory), delete the relevant lines and restart the client.

---

## Known platform issues

- **GtkFileFilter ignored on Windows** — the native file dialog does not honour
  GTK filter rules. Workaround: control available files via what is installed,
  not via filters.
- **Relative paths at runtime** — `CF_DATADIR` is a relative compile-time path
  and is only valid if the working directory is the install root. Use
  `CF_DATADIR_RT` everywhere in source code.
- **`g_getenv("HOME")` on Windows** — returns `C:\Users\<user>`, not the
  MSYS2 home. Code that reads legacy config from `$HOME/.crossfire/` may not
  find files in the MSYS2 home directory.
- **Icon format** — `client.ico` contains only 16x16 and 32x32 images at
  4-bit colour depth. The GTK window icon is loaded separately from `48x48.png`
  to get a higher-quality titlebar icon.

---

## Testing checklist

Before committing any change that touches `config.c`, `main.c`, or a `.ui`
file, verify the following on Windows:

- [ ] Client launches without error dialogs
- [ ] Metaserver browser appears and populates
- [ ] Edit → Preferences opens correctly
- [ ] Theme chooser opens with the currently active `.css` file pre-selected
- [ ] Layout chooser opens with the currently active `.ui` file pre-selected
- [ ] Selecting `black.css` and clicking Apply changes message colours
- [ ] Settings persist correctly after restarting the client
- [ ] `client.ini` `theme` and `window_layout` values are absolute paths after Apply
- [ ] Application icon appears in Explorer, taskbar, and Alt-Tab switcher
- [ ] Start Menu and Desktop shortcuts have the Crossfire icon
### GTK type system on Windows/MSYS2 UCRT64

`GtkHPaned` and `GtkVPaned` are registered as distinct GTypes in the MSYS2
UCRT64 GTK3 build. An exact type comparison such as `type == GTK_TYPE_PANED`
will not match them. Always use `g_type_is_a(type, GTK_TYPE_PANED)` when
checking whether an object is a paned widget. This affects any code that
iterates `gtk_builder_get_objects()` and filters by type, including
`save_winpos()` and `load_window_positions()` in `config.c`.

```c
/* Wrong - misses GtkHPaned and GtkVPaned on Windows */
if (G_OBJECT_TYPE(widget) == GTK_TYPE_PANED) { ... }

/* Correct - matches GtkPaned, GtkHPaned, GtkVPaned */
if (g_type_is_a(G_OBJECT_TYPE(widget), GTK_TYPE_PANED)) { ... }
```

### WIN32 preprocessor macro on MSYS2

MSYS2/UCRT64 defines `_WIN32`, not `WIN32`. All Windows-specific guards must
use `#ifdef _WIN32`. The macro `WIN32` without the leading underscore is not
defined by the MSYS2 compiler and guards using it will be silently skipped.
When reviewing existing code, grep for `#ifdef WIN32` and replace with
`#ifdef _WIN32`. The smoke test `gtk-v2/test-windows-smoke.sh` checks for
this automatically.

### XPM images on Windows

`libpixbufloader-xpm.dll` causes a fatal `cannot register existing type
'GdkPixbuf'` crash on Windows due to a GObject type registration conflict.
Do not include this loader in the Windows deployment bundle. Any code that
calls `gdk_pixbuf_new_from_xpm_data()` must be wrapped in `#ifdef _WIN32` /
`#else` and use `gdk_pixbuf_new_from_inline()` with pre-converted RGBA data
on Windows. Use `gdk-pixbuf-csource` to generate the inline header:

```bash
gdk-pixbuf-csource --name=foo_inline pixmaps/foo.xpm > gtk-v2/src/foo_inline.h
```

Then in the source file:

```c
#ifdef _WIN32
#include "foo_inline.h"
    pb = gdk_pixbuf_new_from_inline(-1, foo_inline, FALSE, NULL);
#else
#include "../../pixmaps/foo.xpm"
    pb = gdk_pixbuf_new_from_xpm_data((const char **)foo_xpm);
#endif
```

---

## Automated smoke test

Run before committing any change to `config.c`, `main.c`, `inventory.c`,
`image.c`, or deploy scripts:

```bash
bash gtk-v2/test-windows-smoke.sh
```

Checks performed: `_WIN32` guard usage, XPM call guards, GtkPaned type check
style, file chooser folder navigation, deploy folder integrity (exe present,
no xpm/svg loaders, clean loaders.cache, gschemas.compiled present).

---

## Testing checklist (updated)

Before committing any change that touches `config.c`, `main.c`, `inventory.c`,
`image.c`, or a `.ui` file, verify the following on Windows:

- [ ] Smoke test passes: `bash gtk-v2/test-windows-smoke.sh`
- [ ] Client launches without error dialogs or GLib-GObject-CRITICAL warnings
- [ ] No `cannot register existing type` errors in stderr
- [ ] Metaserver browser appears and populates
- [ ] Edit -> Preferences opens correctly
- [ ] Theme chooser opens in the themes directory (not the ui directory)
- [ ] Layout chooser opens in the ui directory
- [ ] Selecting black.css and clicking Apply changes message colours
- [ ] Edit -> Save Window Position writes paned positions to client.ini
- [ ] Verify: grep -A 10 "[gtk-v2.ui]" "$APPDATA/Local/crossfire/client.ini"
- [ ] That output must contain vpaned_map_stats, hpaned_client etc, not just window_root
- [ ] Restarting the client restores panel positions from the saved values
- [ ] Settings persist correctly after restarting the client
- [ ] client.ini theme and window_layout values are absolute paths after Apply
- [ ] Application icon appears in Explorer, taskbar, and Alt-Tab switcher
- [ ] Inventory panel tabs show icons (not blank or missing)
- [ ] No XPM loader warnings in stderr output
