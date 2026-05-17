# Building the Crossfire GTK3 Client as a Windows .exe Installer

This document describes how to build the Crossfire GTK3 client as a
self-contained Windows installer (`.exe`) using MSYS2/UCRT64 on Windows 11.
The result is a standalone NSIS-packaged installer that bundles all required
DLLs, data files, and GTK runtime assets.

---

## Prerequisites

### MSYS2

Install [MSYS2](https://www.msys2.org/) to `C:\msys64`. Use the **UCRT64**
shell for all build commands.

Install the required packages:

```bash
pacman -S \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gtk3 \
  mingw-w64-ucrt-x86_64-libcurl \
  mingw-w64-ucrt-x86_64-SDL2 \
  mingw-w64-ucrt-x86_64-SDL2_mixer \
  mingw-w64-ucrt-x86_64-vala \
  nsis
```

### Source tree

```
~/client/windows-client-gtkv3-experiment-sf/   ← git source (branch: gtk3)
~/staging/CrossfireClient/                      ← assembled install tree
~/staging/crossfire-client.nsi                  ← NSIS installer script
```

---

## Build

### Configure (first time only)

```bash
cd ~/client/windows-client-gtkv3-experiment-sf
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Compile

```bash
cd ~/client/windows-client-gtkv3-experiment-sf/build
cmake --build . --config Release
```

The output executable is:

```
build/gtk-v2/src/crossfire-client-gtk3.exe
```

---

## Staging

The staging directory at `~/staging/CrossfireClient/` must be assembled before
packaging. The tree structure is:

```
CrossfireClient/
├── crossfire-client-gtk3.exe
├── client.ico
├── 16x16.png
├── 32x32.png
├── 48x48.png
├── *.dll                              (67 DLLs from /ucrt64/bin/)
├── bin/
│   └── gdbus.exe
├── etc/
│   └── fonts/
├── lib/
│   ├── gtk-3.0/
│   └── gdk-pixbuf-2.0/
└── share/
    ├── glib-2.0/schemas/
    ├── icons/
    │   ├── Adwaita/
    │   └── hicolor/
    └── crossfire-client/
        ├── sounds/
        ├── themes/
        │   ├── standard.css
        │   └── black.css
        └── ui/
            ├── dialogs.ui
            └── *.ui
```

### Copying the executable to staging

```bash
cp ~/client/windows-client-gtkv3-experiment-sf/build/gtk-v2/src/crossfire-client-gtk3.exe \
   ~/staging/CrossfireClient/
```

### Icon assets

The icon files are sourced from `pixmaps/` in the source tree and are copied
to the staging root once:

```bash
cp ~/client/windows-client-gtkv3-experiment-sf/pixmaps/client.ico  ~/staging/CrossfireClient/
cp ~/client/windows-client-gtkv3-experiment-sf/pixmaps/16x16.png   ~/staging/CrossfireClient/
cp ~/client/windows-client-gtkv3-experiment-sf/pixmaps/32x32.png   ~/staging/CrossfireClient/
cp ~/client/windows-client-gtkv3-experiment-sf/pixmaps/48x48.png   ~/staging/CrossfireClient/
```

### Theme files

Only GTK3 CSS themes are installed. The GTK2-format `Standard` and `Black`
files must **not** be present — they cannot be loaded by the GTK3 CSS provider
and would appear as invalid choices in the Preferences dialog.

The themes directory should contain only:

```
share/crossfire-client/themes/standard.css
share/crossfire-client/themes/black.css
```

### UI data files

After modifying any `.ui` file in the source tree, copy it to staging:

```bash
cp ~/client/windows-client-gtkv3-experiment-sf/gtk-v2/ui/dialogs.ui \
   ~/staging/CrossfireClient/share/crossfire-client/ui/
```

---

## Packaging

```bash
cd ~/staging
rm -f CrossfireClient-1.75.3-gtk3-win64.exe
makensis crossfire-client.nsi
```

The installer is written to:

```
~/staging/CrossfireClient-1.75.3-gtk3-win64.exe
```

---

## Installing for testing

Run the installer as Administrator. The default install path is:

```
C:\Program Files\CrossfireClient\
```

To update just the executable without reinstalling:

```powershell
Copy-Item "C:\msys64\home\leaf\staging\CrossfireClient\crossfire-client-gtk3.exe" `
          "C:\Program Files\CrossfireClient\crossfire-client-gtk3.exe"
```

---

## Configuration file location

The client stores its settings at:

```
C:\Users\<username>\AppData\Local\crossfire\client.ini
```

This is determined at runtime by `g_get_user_config_dir()` (which returns
`AppData\Local` on Windows) plus the `crossfire` subdirectory created by
`common/init.c`.

If the `theme` key in `client.ini` points to a stale or invalid path (for
example a GTK2-format theme file from a previous installation), delete the
`theme=` line and restart the client. The client will default to
`standard.css`.

---

## Windows-specific patches

The following changes to the upstream source are required for a working Windows
build. All are guarded by `#ifdef WIN32` or `if(WIN32)` and do not affect
Linux or macOS builds.

### 1. CMake — suppress Unix-only targets (`CMakeLists.txt`)

`find_package(X11 REQUIRED)` and the `.desktop`/hicolor icon install rules are
wrapped in `if(UNIX)` blocks so they are skipped on Windows.

### 2. CMake — Windows resource file (`gtk-v2/src/CMakeLists.txt`)

A Windows resource file `crossfire.rc` is compiled and linked into the
executable on WIN32:

```cmake
if(WIN32)
    set(WIN32_RESOURCES crossfire.rc)
endif()

add_executable(crossfire-client-gtk3
    ${VALA_C}
    ${WIN32_RESOURCES}
    ...
)
```

### 3. Windows resource file (`gtk-v2/src/crossfire.rc`)

Embeds `client.ico` into the compiled executable so Windows displays the
Crossfire icon in Explorer, the taskbar, and the Alt-Tab switcher:

```rc
#include <windows.h>
IDI_ICON1 ICON "..\\..\\pixmaps\\client.ico"
```

### 4. Runtime working directory (`gtk-v2/src/main.c`, `main.h`)

At startup on WIN32, `SetCurrentDirectoryW()` sets the working directory to
the directory containing the executable. This is essential because all
data-file paths are resolved relative to this directory.

A global `cf_datadir_abs[MAX_BUF]` is populated from the exe location and
exposed via the `CF_DATADIR_RT` macro:

```c
/* main.h */
#ifdef WIN32
extern char cf_datadir_abs[MAX_BUF];
#define CF_DATADIR_RT cf_datadir_abs
#else
#define CF_DATADIR_RT CF_DATADIR
#endif
```

`CF_DATADIR_RT` expands to the absolute runtime path on Windows and the
compile-time `CF_DATADIR` on other platforms.

### 5. GTK window icon (`gtk-v2/src/main.c`)

After `window_root` is created, `gtk_window_set_icon_from_file()` loads
`48x48.png` from the install directory (the current working directory set in
step 4):

```c
#ifdef WIN32
{
    gchar *cwd = g_get_current_dir();
    gchar *icon48 = g_build_filename(cwd, "48x48.png", NULL);
    GError *icon_err = NULL;
    gtk_window_set_icon_from_file(GTK_WINDOW(window_root), icon48, &icon_err);
    if (icon_err) g_error_free(icon_err);
    g_free(icon48);
    g_free(cwd);
}
#endif
```

### 6. CSS theme loading (`gtk-v2/src/config.c`)

`init_theme()` and the default `theme` path in `config_load()` use
`CF_DATADIR_RT` to build absolute paths to the CSS theme files:

```c
/* init_theme() */
#ifdef WIN32
    gchar *default_theme = g_build_filename(CF_DATADIR_RT, "themes", "standard.css", NULL);
    apply_theme_css(default_theme);
    g_free(default_theme);
#else
    apply_theme_css(THEME_DEFAULT);
#endif

/* config_load() default */
#ifdef WIN32
    theme = g_build_filename(CF_DATADIR_RT, "themes", "standard.css", NULL);
#endif
```

### 7. Layout file chooser (`gtk-v2/src/config.c`)

The layout (`ui_filechooser`) uses `gtk_file_chooser_set_current_folder()`
with an absolute path on WIN32 instead of `gtk_file_chooser_set_filename()`,
because the GTK file chooser button ignores relative or invalid filenames
silently:

```c
#ifdef WIN32
    gchar *abs_ui_dir = g_build_filename(CF_DATADIR_RT, "ui", NULL);
    gtk_file_chooser_set_current_folder(ui_filechooser, abs_ui_dir);
    g_free(abs_ui_dir);
#else
    gtk_file_chooser_set_filename(ui_filechooser, window_xml_file);
#endif
```

### 8. Theme file chooser (`gtk-v2/src/config.c`)

The theme file chooser (`theme_filechooser`) applies the same fix:

```c
#ifdef WIN32
    gchar *abs_theme_dir = g_build_filename(CF_DATADIR_RT, "themes", NULL);
    gtk_file_chooser_set_current_folder(theme_filechooser, abs_theme_dir);
    g_free(abs_theme_dir);
#else
    gtk_file_chooser_set_filename(theme_filechooser, theme);
#endif
```

> **Note:** The Windows native file dialog ignores GTK `GtkFileFilter` rules.
> Filtering to `.css` only is achieved by not installing the GTK2-format theme
> files (`Standard`, `Black`) rather than by relying on a file filter.

---

## NSIS installer script

The NSIS script at `~/staging/crossfire-client.nsi` handles installation,
shortcuts, and registry entries. Key settings:

| Setting | Value |
|---|---|
| Install directory | `C:\Program Files\CrossfireClient` |
| Installer icon | `client.ico` (from staging root) |
| Uninstaller icon | `client.ico` |
| Start Menu shortcut | `Crossfire\Crossfire Client.lnk` |
| Desktop shortcut | `Crossfire Client.lnk` |
| Shortcut icon | `$INSTDIR\client.ico` |

Both shortcuts pass `$INSTDIR` as the working directory so that
`SetCurrentDirectoryW` in `main.c` finds the correct exe directory at startup.

---

## DLL bundling notes

- 67 DLLs from `/ucrt64/bin/` are bundled in the staging root.
- `gdk-pixbuf` loaders are **not** bundled separately — the built-in GDK
  pixbuf support is sufficient and bundling the loaders directory caused a
  crash on startup.
- The GTK runtime (`lib/gtk-3.0/`, `lib/gdk-pixbuf-2.0/`,
  `share/glib-2.0/schemas/`, `share/icons/hicolor/`, `etc/fonts/`) is
  required for GTK to function without a system GTK installation.

---

## Commit history (Windows compatibility)

| Commit | Summary |
|---|---|
| `cmake, main: Add Windows build compatibility fixes` | X11/desktop guards, `SetCurrentDirectoryW`, `CF_DATADIR_RT` |
| `config, main: Fix layout file chooser path on Windows` | `ui_filechooser` uses `set_current_folder` + `CF_DATADIR_RT` |
| `config: Fix CSS theme loading and default theme path on Windows` | `init_theme()` and `config_load()` use `CF_DATADIR_RT` for CSS paths |
| `cmake, main: Add Windows application icon support` | `crossfire.rc`, CMake WIN32_RESOURCES, `gtk_window_set_icon_from_file` |
| `config: Fix theme filechooser path on Windows` | `theme_filechooser` uses `set_current_folder` + `CF_DATADIR_RT` |