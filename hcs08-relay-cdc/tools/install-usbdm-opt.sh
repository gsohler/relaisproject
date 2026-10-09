#!/bin/sh
# install-usbdm-opt.sh - Installs USBDM from the (partly corrupt) official
# usbdm_4.12.1.340-1-x86_64.deb into /opt/usbdm, without dpkg.
#
# What it does:
#   - extracts everything readable from the .deb into /opt/usbdm
#     (the Eclipse UpdateSite at the end of the archive is corrupt and dropped)
#   - recreates missing shared-library links (libfoo.so.N -> libfoo.so.N.M)
#     from the SONAME of each library, since some links may sit behind the
#     corrupt part of the archive
#   - links /usr/share/usbdm -> /opt/usbdm/usr/share/usbdm (device database)
#   - installs the udev rules (USBDM access without sudo)
#   - creates a wrapper in /usr/local/bin for every USBDM program, which sets
#     LD_LIBRARY_PATH, so "UsbdmFlashProgrammer" and "make flash" just work
#
# Usage:
#   sudo sh install-usbdm-opt.sh /path/to/usbdm_4.12.1.340-1-x86_64.deb
#   sudo sh install-usbdm-opt.sh --uninstall
set -e

PREFIX=/opt/usbdm
BINDIR=/usr/local/bin
MARK="# usbdm-opt wrapper"

[ "$(id -u)" = 0 ] || { echo "Please run with sudo."; exit 1; }

uninstall() {
    for f in "$BINDIR"/*; do
        [ -f "$f" ] && grep -q "$MARK" "$f" 2>/dev/null && rm -f "$f"
    done
    [ -L /usr/share/usbdm ] && rm -f /usr/share/usbdm
    rm -f /etc/udev/rules.d/*usbdm*-opt.rules
    rm -rf "$PREFIX"
    udevadm control --reload-rules 2>/dev/null || true
    echo "USBDM removed from $PREFIX."
}

if [ "$1" = "--uninstall" ]; then
    uninstall
    exit 0
fi

DEB=${1:?usage: sudo sh $0 usbdm_4.12.1.340-1-x86_64.deb | --uninstall}
DEB=$(readlink -f "$DEB")

# A dpkg-installed usbdm (e.g. the repacked one) would conflict.
if dpkg -s usbdm >/dev/null 2>&1; then
    echo "Removing the dpkg-installed usbdm package first ..."
    apt-get -y purge usbdm || dpkg --purge --force-all usbdm
fi
[ -L /usr/share/usbdm ] || [ ! -e /usr/share/usbdm ] || {
    echo "/usr/share/usbdm exists and is not our link - please remove it first."; exit 1; }

uninstall >/dev/null
mkdir -p "$PREFIX"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
ar x "$DEB"

echo "Extracting into $PREFIX (the corrupt end of the archive is skipped) ..."
zstd -dc data.tar.zst 2>/dev/null | tar -x -C "$PREFIX" 2>/dev/null || true
rm -rf "$PREFIX/usr/share/usbdm/UpdateSite"

ls "$PREFIX/usr/bin" 2>/dev/null | grep -qi usbdm || { echo "ERROR: no USBDM programs found"; exit 1; }

# Library directories inside the package
LIBDIRS=$(find "$PREFIX" -name '*.so*' \( -type f -o -type l \) -printf '%h\n' | sort -u | tr '\n' ':')

# Recreate missing SONAME links (libfoo.so.4 -> libfoo.so.4.12.1) and the
# plain development names (libfoo.so -> libfoo.so.4), which USBDM loads
# with dlopen().
find "$PREFIX" -name '*.so*' -type f | while read -r lib; do
    dir=$(dirname "$lib"); base=$(basename "$lib")
    soname=$(readelf -d "$lib" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')
    if [ -n "$soname" ] && [ "$soname" != "$base" ] && [ ! -e "$dir/$soname" ]; then
        ln -s "$base" "$dir/$soname"; echo "  link $soname -> $base"
    fi
    plain=$(echo "$base" | sed 's/\.so\..*/.so/')
    if [ "$plain" != "$base" ] && [ ! -e "$dir/$plain" ]; then
        ln -s "$base" "$dir/$plain"; echo "  link $plain -> $base"
    fi
done

# Device database where USBDM expects it
ln -s "$PREFIX/usr/share/usbdm" /usr/share/usbdm

# udev rules
mkdir -p /etc/udev/rules.d
for r in $(find "$PREFIX" -path '*rules.d*' -name '*.rules' 2>/dev/null); do
    cp "$r" "/etc/udev/rules.d/$(basename "$r" .rules)-opt.rules"
done
udevadm control --reload-rules 2>/dev/null && udevadm trigger 2>/dev/null || true

# Wrappers
for p in "$PREFIX"/usr/bin/*; do
    [ -f "$p" ] && [ -x "$p" ] || continue
    w="$BINDIR/$(basename "$p")"
    cat > "$w" <<EOF
#!/bin/sh
$MARK
export LD_LIBRARY_PATH="$LIBDIRS\${LD_LIBRARY_PATH}"
exec "$p" "\$@"
EOF
    chmod +x "$w"
done

echo
echo "Installed. Programs:"
ls "$PREFIX/usr/bin"
echo
echo "Missing libraries (should be empty):"
for p in "$PREFIX"/usr/bin/*; do
    LD_LIBRARY_PATH="$LIBDIRS" ldd "$p" 2>/dev/null | grep "not found" | sed "s|^|  $(basename "$p"): |"
done || true
