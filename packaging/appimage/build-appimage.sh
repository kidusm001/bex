#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?Usage: $0 <version> [output-dir]}"
OUT_DIR="${2:-$PWD}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

export APPIMAGE_EXTRACT_AND_RUN=1
export VERSION
export ARCH=x86_64

echo "==> Building bex ${VERSION} for AppImage"
cmake -S "${ROOT_DIR}" -B "${WORK_DIR}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBEX_BUILD_TESTS=OFF \
    -DBEX_X11=ON
cmake --build "${WORK_DIR}/build" -j"$(nproc)"
DESTDIR="${WORK_DIR}/AppDir" cmake --install "${WORK_DIR}/build"

# AppImage requires a relative executable name in the desktop entry.
DESKTOP_FILE="${WORK_DIR}/AppDir/usr/share/applications/io.github.kidusm001.bex.desktop"
sed -i 's|^Exec=.*|Exec=bex|' "${DESKTOP_FILE}"

# Fetch linuxdeploy + Qt plugin.
TOOLS_DIR="${WORK_DIR}/tools"
mkdir -p "${TOOLS_DIR}"
curl -fsSL -o "${TOOLS_DIR}/linuxdeploy-x86_64.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
curl -fsSL -o "${TOOLS_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x "${TOOLS_DIR}/linuxdeploy-x86_64.AppImage" "${TOOLS_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage"

if [ -x /usr/lib/qt6/bin/qmake ]; then
    export QMAKE=/usr/lib/qt6/bin/qmake
elif command -v qmake6 >/dev/null 2>&1; then
    export QMAKE="$(command -v qmake6)"
fi

export EXTRA_QT_PLUGINS="platforms;platformthemes;wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration;iconengines"
export OUTPUT="${OUT_DIR}/bex-${VERSION}-x86_64.AppImage"

mkdir -p "${OUT_DIR}"
(
    cd "${WORK_DIR}"
    "${TOOLS_DIR}/linuxdeploy-x86_64.AppImage" \
        --appdir "${WORK_DIR}/AppDir" \
        --desktop-file "${DESKTOP_FILE}" \
        --icon-file "${WORK_DIR}/AppDir/usr/share/icons/hicolor/scalable/apps/io.github.kidusm001.bex.svg" \
        --plugin qt \
        --output appimage
)

chmod +x "${OUTPUT}"
echo "==> Smoke-testing ${OUTPUT}"
QT_QPA_PLATFORM=offscreen "${OUTPUT}" --appimage-extract-and-run --version
echo "==> Built ${OUTPUT}"
