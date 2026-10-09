#!/bin/sh
# repack-usbdm.sh - Rebuilds the broken USBDM 4.12.1.340 .deb into a clean one.
#
# The official usbdm_4.12.1.340-1-x86_64.deb has a corrupt zstd stream near the
# end (inside the Eclipse plugin UpdateSite zip). Everything before that point
# - programs, libraries, device database, udev rules - is intact. This script
# extracts what is readable, drops the Eclipse UpdateSite, and builds a new
# .deb so that all files end up where the programs expect them (no symlinks,
# no LD_LIBRARY_PATH) and dpkg can remove the package cleanly later.
#
# Usage:  sh repack-usbdm.sh /path/to/usbdm_4.12.1.340-1-x86_64.deb
# Result: ./usbdm_4.12.1.340-1fixed_amd64.deb
#         install with: sudo apt install ./usbdm_4.12.1.340-1fixed_amd64.deb
set -e

DEB=${1:?usage: $0 usbdm_4.12.1.340-1-x86_64.deb}
DEB=$(readlink -f "$DEB")
OUT=$PWD/usbdm_4.12.1.340-1fixed_amd64.deb
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
ar x "$DEB"

mkdir -p pkg/DEBIAN
# Control part is intact.
case $(ls control.tar.*) in
  *.zst) zstd -dc control.tar.zst ;;
  *.xz)  xz -dc control.tar.xz ;;
  *.gz)  gzip -dc control.tar.gz ;;
esac | tar -x -C pkg/DEBIAN

# Data part: extract as far as the stream is readable; the error at the end
# is expected and ignored.
echo "Extracting data (an error about corrupt data at the end is expected) ..."
zstd -dc data.tar.zst 2>/dev/null | tar -x -C pkg 2>/dev/null || true

# Drop the damaged Eclipse plugin and the checksums of the original package.
rm -rf pkg/usr/share/usbdm/UpdateSite
rm -f pkg/DEBIAN/md5sums

# New version string so apt does not confuse it with the original.
sed -i 's/^Version:.*/Version: 4.12.1.340-1fixed/' pkg/DEBIAN/control

# Sanity check: the programs and the device database must be there.
ls pkg/usr/bin | grep -qi usbdm || { echo "ERROR: no USBDM programs extracted"; exit 1; }
[ -d pkg/usr/share/usbdm ] || { echo "ERROR: /usr/share/usbdm missing"; exit 1; }

dpkg-deb --root-owner-group -b pkg "$OUT" >/dev/null
echo "Built: $OUT"
echo "Install with: sudo apt install $OUT"
