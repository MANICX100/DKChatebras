#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-v4}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"
DIST_DIR="${PROJECT_ROOT}/build/dist"
STAGE_NAME="DKChatebras-${VERSION}-linux-${ARCH}"
STAGE_ROOT="${DIST_DIR}/stage"
STAGE_DIR="${STAGE_ROOT}/${STAGE_NAME}"

make -C "${PROJECT_ROOT}" clean
make -C "${PROJECT_ROOT}" release
make -C "${PROJECT_ROOT}" test

rm -rf "${DIST_DIR}"
mkdir -p "${STAGE_DIR}/data/icons/hicolor/scalable/apps"

install -Dm755 "${PROJECT_ROOT}/build/dkchatebras" "${STAGE_DIR}/dkchatebras"
install -Dm644 "${PROJECT_ROOT}/data/io.github.dkchatebras.DKChatebras.desktop" \
  "${STAGE_DIR}/data/io.github.dkchatebras.DKChatebras.desktop"
install -Dm644 "${PROJECT_ROOT}/data/io.github.dkchatebras.DKChatebras.metainfo.xml" \
  "${STAGE_DIR}/data/io.github.dkchatebras.DKChatebras.metainfo.xml"
install -Dm644 "${PROJECT_ROOT}/data/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg" \
  "${STAGE_DIR}/data/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg"
install -Dm644 "${PROJECT_ROOT}/README.md" "${STAGE_DIR}/README.md"
install -Dm644 "${PROJECT_ROOT}/COPYING" "${STAGE_DIR}/COPYING"

cat > "${STAGE_DIR}/install-user.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
install -Dm755 "${DIR}/dkchatebras" "${PREFIX}/bin/dkchatebras"
install -Dm644 "${DIR}/data/io.github.dkchatebras.DKChatebras.desktop" \
  "${PREFIX}/share/applications/io.github.dkchatebras.DKChatebras.desktop"
install -Dm644 "${DIR}/data/io.github.dkchatebras.DKChatebras.metainfo.xml" \
  "${PREFIX}/share/metainfo/io.github.dkchatebras.DKChatebras.metainfo.xml"
install -Dm644 "${DIR}/data/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg" \
  "${PREFIX}/share/icons/hicolor/scalable/apps/io.github.dkchatebras.DKChatebras.svg"
echo "Installed DKChatebras to ${PREFIX}/bin/dkchatebras"
EOF
chmod +x "${STAGE_DIR}/install-user.sh"

# Standalone executable + portable tarball
install -Dm755 "${PROJECT_ROOT}/build/dkchatebras" "${DIST_DIR}/${STAGE_NAME}"
tar -C "${STAGE_ROOT}" -czf "${DIST_DIR}/${STAGE_NAME}.tar.gz" "${STAGE_NAME}"
rm -rf "${STAGE_ROOT}"

ls -lh "${DIST_DIR}/${STAGE_NAME}" "${DIST_DIR}/${STAGE_NAME}.tar.gz"
