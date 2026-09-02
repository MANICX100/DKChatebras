#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-appimage"
APPDIR="${PROJECT_ROOT}/AppDir"
TOOLS_DIR="${PROJECT_ROOT}/.appimage-tools"
ARCH="$(uname -m)"

case "${ARCH}" in
  x86_64) LINUXDEPLOY_ARCH="x86_64" ;;
  aarch64) LINUXDEPLOY_ARCH="aarch64" ;;
  *) echo "Unsupported AppImage architecture: ${ARCH}" >&2; exit 1 ;;
esac

LINUXDEPLOY="${TOOLS_DIR}/linuxdeploy-${LINUXDEPLOY_ARCH}.AppImage"
GTK_PLUGIN="${TOOLS_DIR}/linuxdeploy-plugin-gtk.sh"

mkdir -p "${TOOLS_DIR}"
if [[ ! -x "${LINUXDEPLOY}" ]]; then
  curl -fL "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${LINUXDEPLOY_ARCH}.AppImage" -o "${LINUXDEPLOY}"
  chmod +x "${LINUXDEPLOY}"
fi
if [[ ! -x "${GTK_PLUGIN}" ]]; then
  curl -fL "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" -o "${GTK_PLUGIN}"
  chmod +x "${GTK_PLUGIN}"
fi
ln -sf "${GTK_PLUGIN}" "${TOOLS_DIR}/linuxdeploy-plugin-gtk"
export PATH="${TOOLS_DIR}:${PATH}"

rm -rf "${BUILD_DIR}" "${APPDIR}"
meson setup "${BUILD_DIR}" "${PROJECT_ROOT}" --buildtype=release --prefix=/usr
meson compile -C "${BUILD_DIR}"
DESTDIR="${APPDIR}" meson install -C "${BUILD_DIR}"

export DEPLOY_GTK_VERSION=4
export OUTPUT="${PROJECT_ROOT}/DKChatebras-${ARCH}.AppImage"
"${LINUXDEPLOY}" \
  --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/dkchatebras" \
  --desktop-file "${APPDIR}/usr/share/applications/io.github.dkchatebras.DKChatebras.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg" \
  --plugin gtk \
  --output appimage

echo "Created ${OUTPUT}"
