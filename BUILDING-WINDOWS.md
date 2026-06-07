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
  mingw-w64-ucrt-x86_64-ntldd-git \
  nsis
```

### Source tree

Clone the repository and check out the branch:

```bash
git clone git@<server>:<user>/windows-client-gtkv3-experiment-sf.git \
  ~/crossfire-gtk3-experiment
cd ~/crossfire-gtk3-experiment
```

---

## Build

### Configure (first time only)

```bash
cd ~/crossfire-gtk3-experiment
mkdir -p build-ucrt64 && cd build-ucrt64
cmake .. -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release
```

### Compile

```bash
cd ~/crossfire-gtk3-experiment/build-ucrt64
make -j$(nproc)
```

The output executable is:

```
build-ucrt64/gtk-v2/src/crossfire-client-gtk3.exe
```

---

## Deploy folder

The deploy folder is a self-contained directory that can be run directly or
packaged into an installer. Assemble it as follows.

### 1. Create the deploy folder and copy the executable

```bash
DEPLOY=~/crossfire-gtk3-deploy
mkdir -p "$DEPLOY"
EXE=$(find ~/crossfire-gtk3-experiment/build-ucrt64 -name "crossfire-client-gtk3.exe" | head -1)
cp "$EXE" "$DEPLOY/"
```

### 2. Copy all required UCRT64 DLLs

```bash
ntldd -R "$EXE" \
  | grep "ucrt64" \
  | awk '{print $3}' \
  | sed 's|\\|/|g' \
  | sed 's|C:/msys64|/c/msys64|g' \
  | sort -u \
  | xargs -I{} cp {} "$DEPLOY/"
```

### 3. Copy icon assets

```bash
cp ~/crossfire-gtk3-experiment/pixmaps/client.ico "$DEPLOY/"
cp ~/crossfire-gtk3-experiment/pixmaps/16x16.png  "$DEPLOY/"
cp ~/crossfire-gtk3-experiment/pixmaps/32x32.png  "$DEPLOY/"
cp ~/crossfire-gtk3-experiment/pixmaps/48x48.png  "$DEPLOY/"
```

### 4. Copy GTK runtime data

```bash
# GLib schemas (required - GTK crashes without this)
mkdir -p "$DEPLOY/share/glib-2.0/schemas"
cp /ucrt64/share/glib-2.0/schemas/gschemas.compiled \
   "$DEPLOY/share/glib-2.0/schemas/"

# Icon themes
mkdir -p "$DEPLOY/share/icons"
cp -r /ucrt64/share/icons/Adwaita "$DEPLOY/share/icons/"
cp -r /ucrt64/share/icons/hicolor "$DEPLOY/share/icons/"

# GDK pixbuf loaders
mkdir -p "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders"
cp /ucrt64/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll \
   "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders/"

# Remove loaders that cause GObject type registration crashes at startup
rm -f "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-xpm.dll"
rm -f "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll"
rm -f "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders/"*.dll.a

# Regenerate loaders cache pointing to deploy folder paths
GDK_PIXBUF_MODULEDIR="$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders" \
  /ucrt64/bin/gdk-pixbuf-query-loaders.exe \
  "$DEPLOY"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll \
  > "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
```

### 5. Copy client share data

```bash
# Run cmake install to populate build-ucrt64/share/
cd ~/crossfire-gtk3-experiment/build-ucrt64
cmake -P cmake_install.cmake

# Copy to deploy
cp -r ~/crossfire-gtk3-experiment/build-ucrt64/share/. "$DEPLOY/share/"

# Remove GTK2-format theme files installed by cmake
# These appear alongside the CSS files in the theme chooser and break theming
rm -f "$DEPLOY/share/crossfire-client/themes/Black"
rm -f "$DEPLOY/share/crossfire-client/themes/Standard"

# Remove the sounds submodule's repo metadata
# The sounds/ directory is a git submodule, so cmake install also copies its
# .git directory and .gitignore. These add unnecessary bloat to the deploy
# folder and installer and must not be shipped.
rm -rf "$DEPLOY/share/crossfire-client/sounds/.git"
rm -f "$DEPLOY/share/crossfire-client/sounds/.gitignore"
```

### 6. Create the launcher script

```bash
cat > "$DEPLOY/launch.sh" << 'LAUNCHEOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PATH="$SCRIPT_DIR:$PATH"
export GDK_PIXBUF_MODULE_FILE="$SCRIPT_DIR/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GDK_PIXBUF_MODULEDIR="$SCRIPT_DIR/lib/gdk-pixbuf-2.0/2.10.0/loaders"
cd "$SCRIPT_DIR"
./crossfire-client-gtk3.exe "$@"
LAUNCHEOF
chmod +x "$DEPLOY/launch.sh"
```

### 7. Verify the deploy folder

```bash
cd ~/crossfire-gtk3-experiment
bash gtk-v2/test-windows-smoke.sh
```

All 13 checks must pass before packaging.

---

## DLL notes

- Approximately 67 DLLs from `/ucrt64/bin/` are bundled in the deploy root.
- `libpixbufloader-xpm.dll` and `pixbufloader_svg.dll` must **not** be
  included - both cause a fatal `cannot register existing type 'GdkPixbuf'`
  crash at startup due to a GObject type registration conflict in the
  MSYS2/UCRT64 GTK3 build.
- `librsvg-2-2.dll` must **not** be included for the same reason.
- The loaders cache must be regenerated after any change to the loaders
  directory, using `gdk-pixbuf-query-loaders.exe` with the deploy folder
  paths, not the `/ucrt64` paths.

---

## Theme files

Only GTK3 CSS themes are installed. The deploy folder must contain:

```
share/crossfire-client/themes/standard.css
share/crossfire-client/themes/black.css
```

The GTK2-format `Standard` and `Black` files are installed by `cmake -P
cmake_install.cmake` and must be explicitly removed from the deploy folder
(see step 5). If present they appear in the theme chooser alongside the CSS
files and cause colorized text to stop working when selected.

---

## Sounds

The `sounds/` directory is checked out as a git submodule. When `cmake -P
cmake_install.cmake` installs `share/crossfire-client/sounds/`, it copies
the submodule's working tree verbatim, including its `.git` directory and
`.gitignore` file. Neither belongs in a deploy folder or installer - they
add unnecessary size and ship repository metadata to end users.

Always remove them after copying the share data into the deploy folder
(see step 5):

```bash
rm -rf "$DEPLOY/share/crossfire-client/sounds/.git"
rm -f "$DEPLOY/share/crossfire-client/sounds/.gitignore"
```

Only `sounds.conf` and the `.wav`/audio asset files should remain.

---

## NSIS packaging

### Build the installer

```bash
DEPLOY=~/crossfire-gtk3-deploy
GITVER=$(cd ~/crossfire-gtk3-experiment && git describe --tags --always)

cd ~/crossfire-gtk3-experiment/gtk-v2/win32
makensis \
  -DVERSION=1.75.3.0 \
  "-DGITVERSION=$GITVER" \
  "-DINPUTDIR=$(cygpath -w $DEPLOY)" \
  "-DOUTPUTDIR=$(cygpath -w $HOME)" \
  client.nsi
```

The installer is written to:

```
~/CrossfireClient-git-<GITVER>.exe
```

### Installing for testing

Run the installer as Administrator. The default install path is:

```
C:\Program Files\Crossfire Client\
```

If you have previously installed the client to a different directory, clear
the stale theme path from `client.ini` before launching, otherwise all text
will appear black:

```bash
sed -i '/^theme=/d' "/c/Users/$USERNAME/AppData/Local/crossfire/client.ini"
```

To update just the executable without reinstalling:

```powershell
Copy-Item "$env:USERPROFILE\crossfire-gtk3-deploy\crossfire-client-gtk3.exe" `
          "C:\Program Files\Crossfire Client\crossfire-client-gtk3.exe"
```

---

## Configuration file location

The client stores its settings at:

```
C:\Users\<username>\AppData\Local\crossfire\client.ini
```

If the `theme` or `window_layout` keys point to stale paths after
reinstalling to a different directory, delete those lines and restart
the client. The client will use default values.

**Important when changing the install directory:** If a previous install
used a different directory name (for example `CrossfireClient` instead of
`Crossfire Client`), the saved `theme=` path in `client.ini` will point to
the old location and the client will silently load no theme, causing all
text to appear black. Delete the `theme=` line before launching:

```bash
sed -i '/^theme=/d' "/c/Users/$USERNAME/AppData/Local/crossfire/client.ini"
```

Or manually edit `client.ini` in Notepad and delete the `theme=` line.
The client will default to `standard.css` from the new install location
on next launch.

---

## Windows-specific implementation notes

See `GTK3_PORT_CHANGES.md` for the full GTK3 port description. The following
Windows-specific issues are worth noting for future maintenance:

- All Windows guards use `#ifdef _WIN32` - MSYS2/UCRT64 defines `_WIN32`,
  not `WIN32`. Guards using bare `WIN32` are silently skipped.
- `CF_DATADIR_RT` provides the runtime-absolute data directory path on
  Windows. Never use `CF_DATADIR` directly in source code.
- `SetCurrentDirectoryW()` is called at startup to set the working directory
  to the directory containing the executable, so all relative data paths
  resolve correctly when launched from a shortcut.
- File chooser dialogs use `gtk_file_chooser_set_current_folder()` on Windows
  because the native dialog silently ignores `gtk_file_chooser_set_filename()`.
- Window position save/load uses `g_type_is_a(type, GTK_TYPE_PANED)` instead
  of `type == GTK_TYPE_PANED` because `GtkHPaned` and `GtkVPaned` are
  registered as distinct GTypes on Windows and do not match `GTK_TYPE_PANED`
  exactly.
- XPM images are converted to inline RGBA data via `gdk-pixbuf-csource`
  because the XPM pixbuf loader plugin causes a fatal startup crash on Windows.
