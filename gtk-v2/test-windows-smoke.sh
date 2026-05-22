#!/bin/bash
# Smoke tests for Windows-specific build correctness.
# Run from the repository root on MSYS2 UCRT64 before committing.
# Does not require a running server.

PASS=0
FAIL=0
SRC="gtk-v2/src"
DEPLOY="${1:-$HOME/crossfire-gtk3-deploy}"

ok()   { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

echo "=== Crossfire Windows smoke tests ==="
echo ""

# 1. No bare #ifdef WIN32 (must be _WIN32)
echo "--- Preprocessor guards ---"
COUNT=$(grep -rn "#ifdef WIN32\b" "$SRC/" 2>/dev/null | wc -l | tr -d ' ')
if [ "$COUNT" -gt 0 ]; then
    fail "#ifdef WIN32 found ($COUNT occurrences) — must be #ifdef _WIN32 for MSYS2"
else
    ok "All WIN32 guards use _WIN32"
fi

# 2. .c files that call gdk_pixbuf_new_from_xpm_data must have a _WIN32 guard
# inv_pixbufs.h is excluded — it is inline data, not a call site
echo "--- XPM usage guarded ---"
FAIL_XPM=0
for f in $(grep -rl "gdk_pixbuf_new_from_xpm_data" "$SRC/" 2>/dev/null | grep '\.c$'); do
    if ! grep -q "#ifdef _WIN32" "$f"; then
        fail "No _WIN32 guard in $f which calls gdk_pixbuf_new_from_xpm_data"
        FAIL_XPM=$((FAIL_XPM+1))
    fi
done
if [ "$FAIL_XPM" -eq 0 ]; then
    ok "All .c files with XPM pixbuf calls contain _WIN32 guards"
fi

# 3. g_type_is_a used for paned type checks
echo "--- GtkPaned type checks ---"
COUNT=$(grep -c "== GTK_TYPE_PANED" "$SRC/config.c" 2>/dev/null | tr -d ' \n')
if [ "${COUNT:-0}" -gt 0 ]; then
    fail "Exact GTK_TYPE_PANED comparison found — use g_type_is_a() instead"
else
    ok "Paned type checks use g_type_is_a()"
fi

# 4. theme_filechooser uses set_current_folder on _WIN32
echo "--- Theme filechooser ---"
COUNT=$(grep -c "set_current_folder(theme_filechooser" "$SRC/config.c" 2>/dev/null | tr -d ' \n')
if [ "${COUNT:-0}" -gt 0 ]; then
    ok "theme_filechooser uses set_current_folder"
else
    fail "theme_filechooser may be using set_filename on Windows"
fi

# 5. ui_filechooser uses set_current_folder on _WIN32
echo "--- UI filechooser ---"
COUNT=$(grep -c "set_current_folder(ui_filechooser" "$SRC/config.c" 2>/dev/null | tr -d ' \n')
if [ "${COUNT:-0}" -gt 0 ]; then
    ok "ui_filechooser uses set_current_folder"
else
    fail "ui_filechooser may be using set_filename on Windows"
fi

# 6. Deploy exe exists
echo "--- Deploy folder ---"
if [ -f "$DEPLOY/crossfire-client-gtk3.exe" ]; then
    ok "Deploy exe exists"
else
    fail "Deploy exe missing: $DEPLOY/crossfire-client-gtk3.exe"
fi

# 7. XPM loader not in deploy
echo "--- XPM loader absent ---"
if [ -f "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-xpm.dll" ]; then
    fail "libpixbufloader-xpm.dll present in deploy — will cause crash"
else
    ok "libpixbufloader-xpm.dll absent from deploy"
fi

# 8. SVG loader not in deploy
echo "--- SVG loader absent ---"
if [ -f "$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll" ]; then
    fail "pixbufloader_svg.dll present in deploy — will cause crash"
else
    ok "pixbufloader_svg.dll absent from deploy"
fi

# 9. loaders.cache exists and has no xpm/svg entries
echo "--- loaders.cache clean ---"
CACHE="$DEPLOY/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
if [ ! -f "$CACHE" ]; then
    fail "loaders.cache missing from deploy"
elif grep -qi "xpm\|svg" "$CACHE"; then
    fail "loaders.cache contains xpm or svg entries"
else
    ok "loaders.cache exists and contains no xpm/svg entries"
fi

# 10. GLib schemas compiled file exists
echo "--- GLib schemas ---"
if [ -f "$DEPLOY/share/glib-2.0/schemas/gschemas.compiled" ]; then
    ok "gschemas.compiled present in deploy"
else
    fail "gschemas.compiled missing from deploy"
fi

echo ""

# 13. sounds.conf present in deploy
echo "--- sounds.conf present ---"
if [ -f "$DEPLOY/share/crossfire-client/sounds/sounds.conf" ]; then
    ok "sounds.conf present in deploy"
else
    fail "sounds.conf missing from deploy - sound will not work"
fi

# 11. client.ico present in deploy
echo "--- client.ico present ---"
if [ -f "$DEPLOY/client.ico" ]; then
    ok "client.ico present in deploy root"
else
    fail "client.ico missing from deploy - icon will not appear in installer"
fi

# 12. No GTK2 theme files in deploy
echo "--- No GTK2 theme files ---"
if ls "$DEPLOY/share/crossfire-client/themes/Black" "$DEPLOY/share/crossfire-client/themes/Standard" 2>/dev/null | grep -q .; then
    fail "GTK2 theme files present in deploy - will appear in theme chooser and break theming"
else
    ok "No GTK2 theme files in deploy themes directory"
fi
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
