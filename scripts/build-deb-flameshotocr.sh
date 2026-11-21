#!/bin/sh
# Build an installable Debian package for FlameshotOCR.
# It shares the source tree with Flameshot but installs under distinct
# paths, so it can be installed alongside the stock flameshot package.
set -eu

SRC_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$SRC_DIR/build-ocr"}
STAGE_DIR=${STAGE_DIR:-"$SRC_DIR/build-ocr-stage"}
PKG_NAME=flameshot-ocr
BASE_VERSION=$(grep -oP 'set\(FLAMESHOT_VERSION \K[0-9.]+' \
    "$SRC_DIR/CMakeLists.txt" | head -1)
PKG_VERSION="${BASE_VERSION}-ocrmod9"
# Shown in the About window so builds are distinguishable (v15.0.0-ocrmod5).
VERSION_SUFFIX=${PKG_VERSION#"$BASE_VERSION"}
ARCH=$(dpkg --print-architecture)
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
OUT_DIR="$SRC_DIR/dist"

cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DFLAMESHOT_APP_NAME=flameshotocr \
    -DFLAMESHOT_DISPLAY_NAME=FlameshotOCR \
    -DFLAMESHOT_DISPLAY_NAME_ZH=火焰截图OCR \
    -DFLAMESHOT_DBUS_NAME=org.flameshotocr.FlameshotOCR \
    -DUSE_KDSINGLEAPPLICATION=OFF \
    -DDISABLE_UPDATE_CHECKER=ON \
    -DFLAMESHOT_VERSION_SUFFIX=${VERSION_SUFFIX}
cmake --build "$BUILD_DIR" -j"$(nproc)"

rm -rf "$STAGE_DIR"
DESTDIR="$STAGE_DIR" cmake --install "$BUILD_DIR"

# Drop the vendored Qt-Color-Widgets development files (headers, static lib,
# cmake/pkg-config metadata). Only the application is packaged.
rm -rf "$STAGE_DIR/usr/include" \
       "$STAGE_DIR/usr/lib/$MULTIARCH/cmake/QtColorWidgets" \
       "$STAGE_DIR/usr/lib/$MULTIARCH/pkgconfig/QtColorWidgets.pc" \
       "$STAGE_DIR/usr/lib/$MULTIARCH/libQtColorWidgets.a"
find "$STAGE_DIR/usr/lib" -type d -empty -delete 2>/dev/null || true

mkdir -p "$STAGE_DIR/DEBIAN"

# Let dpkg work out the library dependencies so the package also installs on
# distributions that use different Qt package names (t64 suffix or not).
SHLIBS=$(cd "$STAGE_DIR" && mkdir -p debian &&
    printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' \
        "$PKG_NAME" "$PKG_NAME" > debian/control &&
    dpkg-shlibdeps -O "usr/bin/flameshotocr" 2>/dev/null |
        sed -n 's/^shlibs:Depends=//p')
rm -rf "$STAGE_DIR/debian"
[ -n "$SHLIBS" ] || SHLIBS="libc6, libstdc++6"

cat > "$STAGE_DIR/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $PKG_VERSION
Section: graphics
Priority: optional
Architecture: $ARCH
Maintainer: FlameshotOCR Contributors <flameshotocr@localhost>
Installed-Size: $(du -sk "$STAGE_DIR" | cut -f1)
Depends: ${SHLIBS}, qt6-svg-plugins, qt6-qpa-plugins, hicolor-icon-theme
Recommends: tesseract-ocr, tesseract-ocr-chi-sim,
 xdg-desktop-portal-gtk | xdg-desktop-portal-gnome | xdg-desktop-portal-kde | xdg-desktop-portal-wlr
Suggests: python3-pip
Homepage: https://flameshot.org/
Description: FlameshotOCR screenshot tool with OCR text extraction
 A Flameshot fork that adds an OCR tool button to the capture editor.
 The selected area is passed to rapidocr, paddleocr or tesseract and the
 recognised text is copied to the clipboard.
 .
 It is co-installable with the stock flameshot package: the binary, desktop
 entry, icons, D-Bus name, config directory and data directory are all
 namespaced as flameshotocr.
EOF

cat > "$STAGE_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database -q /usr/share/applications || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
EOF

cat > "$STAGE_DIR/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database -q /usr/share/applications || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
EOF
chmod 0755 "$STAGE_DIR/DEBIAN/postinst" "$STAGE_DIR/DEBIAN/postrm"

mkdir -p "$OUT_DIR"
dpkg-deb --root-owner-group --build \
    "$STAGE_DIR" "$OUT_DIR/${PKG_NAME}_${PKG_VERSION}_${ARCH}.deb"

echo "Built: $OUT_DIR/${PKG_NAME}_${PKG_VERSION}_${ARCH}.deb"