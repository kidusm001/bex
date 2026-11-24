#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?Usage: $0 <X.Y.Z> [--tag]}"
DO_TAG="${2:-}"

if [[ ! "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: version must be semantic X.Y.Z (got '${VERSION}')" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

TODAY_ISO="$(date -u +%Y-%m-%d)"
TODAY_RPM="$(LC_ALL=C date -u '+%a %b %d %Y')"
TODAY_DEB="$(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S +0000')"

# 1. CMakeLists.txt
sed -i -E "s/^(\s*VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1${VERSION}/" CMakeLists.txt

# 2. flake.nix
sed -i -E "s/(version = \")[0-9]+\.[0-9]+\.[0-9]+(\";)/\1${VERSION}\2/" flake.nix

# 3. packaging/aur/PKGBUILD
sed -i -E "s/^pkgver=.*/pkgver=${VERSION}/" packaging/aur/PKGBUILD
sed -i -E "s/^pkgrel=.*/pkgrel=1/" packaging/aur/PKGBUILD

# 4. packaging/rpm/bex.spec
sed -i -E "s/^(Version:\s+)[0-9]+\.[0-9]+\.[0-9]+/\1${VERSION}/" packaging/rpm/bex.spec
if ! grep -q " - ${VERSION}-1$" packaging/rpm/bex.spec; then
    python3 - "${VERSION}" "${TODAY_RPM}" <<'PY'
import sys
ver, day = sys.argv[1], sys.argv[2]
path = "packaging/rpm/bex.spec"
text = open(path, encoding="utf-8").read()
marker = "%changelog\n"
entry = f"* {day} Kidus Mesfin <kidusmesfin27@gmail.com> - {ver}-1\n- Release v{ver}\n\n"
assert marker in text
open(path, "w", encoding="utf-8").write(text.replace(marker, marker + entry, 1))
PY
fi

# 5. packaging/debian/changelog
if ! head -n 1 packaging/debian/changelog | grep -q "(${VERSION}-1)"; then
    python3 - "${VERSION}" "${TODAY_DEB}" <<'PY'
import sys
ver, day = sys.argv[1], sys.argv[2]
path = "packaging/debian/changelog"
old = open(path, encoding="utf-8").read()
entry = (
    f"bex ({ver}-1) unstable; urgency=medium\n\n"
    f"  * Release v{ver}.\n\n"
    f" -- Kidus Mesfin <kidusmesfin27@gmail.com>  {day}\n\n"
)
open(path, "w", encoding="utf-8").write(entry + old)
PY
fi

# 6. data/io.github.kidusm001.bex.metainfo.xml
if ! grep -q "version=\"${VERSION}\"" data/io.github.kidusm001.bex.metainfo.xml; then
    python3 - "${VERSION}" "${TODAY_ISO}" <<'PY'
import sys
ver, day = sys.argv[1], sys.argv[2]
path = "data/io.github.kidusm001.bex.metainfo.xml"
text = open(path, encoding="utf-8").read()
marker = "  <releases>\n"
entry = f'    <release version="{ver}" date="{day}"/>\n'
assert marker in text
open(path, "w", encoding="utf-8").write(text.replace(marker, marker + entry, 1))
PY
fi

echo "Bumped version to ${VERSION} across CMakeLists.txt, flake.nix, metainfo.xml, PKGBUILD, bex.spec, and debian/changelog."
if [[ "${DO_TAG}" == "--tag" ]]; then
    git add CMakeLists.txt flake.nix data/io.github.kidusm001.bex.metainfo.xml \
        packaging/aur/PKGBUILD packaging/rpm/bex.spec packaging/debian/changelog
    git commit -m "chore(release): prepare v${VERSION}"
    git tag -a "v${VERSION}" -m "bex v${VERSION}"
    echo "Committed and tagged v${VERSION}. Run: git push origin main --tags"
fi
