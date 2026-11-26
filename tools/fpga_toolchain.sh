#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The open FPGA toolchain for open-tick's Tang Nano 20K bridge, reproducible
# and without sudo. Everything goes into one user cache; the system Python,
# ~/.local and the system packages are never touched.
#
#   1. a venv with yowasp-yosys, yowasp-nextpnr-himbaechel-gowin, Apycula and
#      pyserial, every file pinned by SHA-256 (fpga-requirements.lock,
#      pip --require-hashes, wheels only)
#   2. openFPGALoader, built from its release tarball pinned by SHA-256
#      (it programs the board; pip has no package for it)
#   3. a report of what it cannot install itself: the -dev packages that
#      openFPGALoader needs to build, the udev rule and the plugdev group
#      for the BL616's USB interface. It prints the apt command; it never
#      runs sudo.
#
# Usage:
#   tools/fpga_toolchain.sh                    install what is missing, then print the environment line
#   tools/fpga_toolchain.sh --check            install nothing; exit 1 if anything is missing
#   tools/fpga_toolchain.sh --synthesis-only   only the venv: building, not programming (CI)
#
# The directory is $OTK_FPGA_DIR, or ${XDG_CACHE_HOME:-~/.cache}/open-tick/fpga:
#   venv/                         the pip toolchain (and pyserial)
#   openFPGALoader-<version>/bin/ the programmer
#   yowasp-cache/                 yowasp's compiled WebAssembly (YOWASP_CACHE_DIR)
#   MANIFEST.txt                  what was installed, from which pins
# Messages go to stderr; stdout is only the PATH line, so a script can do
#   eval "$(tools/fpga_toolchain.sh)"
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DIR=${OTK_FPGA_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/open-tick/fpga}
LOCK=$here/fpga-requirements.lock

OFL_VERSION=1.1.1
OFL_URL=https://github.com/trabucayre/openFPGALoader/archive/refs/tags/v$OFL_VERSION.tar.gz
OFL_SHA256=ca965f933c52a2a9dbb318df4d4de70fac5f095a8e64523f81036ab467a4b567
OFL_PREFIX=$DIR/openFPGALoader-$OFL_VERSION
# pkg-config module -> Debian/Ubuntu package, for openFPGALoader's build
OFL_DEPS=(libftdi1:libftdi1-dev libusb-1.0:libusb-1.0-0-dev hidapi-hidraw:libhidapi-dev libudev:libudev-dev zlib:zlib1g-dev)

check=0
synthesis_only=0
for arg in "$@"; do
    case $arg in
        --check)          check=1 ;;
        --synthesis-only) synthesis_only=1 ;;
        -h|--help)        sed -n '4,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                echo "fpga_toolchain.sh: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

ok()   { printf '  ok       %s\n' "$*" >&2; }
miss() { printf '  MISSING  %s\n' "$*" >&2; }
note() { printf '  note     %s\n' "$*" >&2; }
missing=0

# Every Python call runs isolated from the user's environment: no PYTHONPATH
# (ROS sets one), no user site-packages, no pip configuration files.
py_clean() { env -u PYTHONPATH -u PYTHONHOME PYTHONNOUSERSITE=1 PIP_CONFIG_FILE=/dev/null "$@"; }

lock_sha=$(sha256sum "$LOCK" | cut -d' ' -f1)
VENV=$DIR/venv

# ---- 1. the pip toolchain --------------------------------------------------
venv_ok() { [[ -f $VENV/.lock.sha256 && $(cat "$VENV/.lock.sha256") == "$lock_sha" ]]; }
if venv_ok; then
    ok "venv $VENV (requirements.lock ${lock_sha:0:12}…)"
elif [[ $check == 1 ]]; then
    miss "venv $VENV, or it was built from another requirements.lock"
    missing=1
else
    command -v python3 >/dev/null || { echo "fpga_toolchain.sh: python3 is needed" >&2; exit 2; }
    # The lock pins wheels (numpy 2.2.6 among them) that exist for CPython 3.10 to 3.13 only.
    pyv=$(py_clean python3 -I -c 'import sys; print("%d.%d" % sys.version_info[:2])')
    case $pyv in
        3.10|3.11|3.12|3.13) ;;
        *) echo "fpga_toolchain.sh: python3 is $pyv; the lock needs CPython 3.10 to 3.13 (put one first on PATH)" >&2; exit 2 ;;
    esac
    echo "fpga_toolchain.sh: creating $VENV from requirements.lock (about 380 MB)" >&2
    mkdir -p "$DIR"
    # Built in place (a venv's scripts hard-code its path). The marker is
    # written last: an interrupted install has none and is rebuilt.
    rm -rf "$VENV"
    py_clean python3 -I -m venv "$VENV"
    py_clean "$VENV/bin/python" -I -m pip install --isolated --disable-pip-version-check --no-input \
        --require-hashes --only-binary :all: -r "$LOCK" >&2
    echo "$lock_sha" > "$VENV/.lock.sha256"
    ok "venv $VENV"
fi

# ---- 2. openFPGALoader ------------------------------------------------------
if [[ $synthesis_only == 0 ]]; then
dep_missing=()
for d in "${OFL_DEPS[@]}"; do
    pkg-config --exists "${d%%:*}" 2>/dev/null || dep_missing+=("${d#*:}")
done
if [[ -x $OFL_PREFIX/bin/openFPGALoader ]]; then
    ok "openFPGALoader $OFL_VERSION in $OFL_PREFIX"
elif [[ $check == 1 ]]; then
    miss "openFPGALoader $OFL_VERSION in $OFL_PREFIX"
    missing=1
elif [[ ${#dep_missing[@]} -gt 0 ]] || ! command -v cmake >/dev/null || ! command -v cc >/dev/null; then
    miss "openFPGALoader cannot be built: missing ${dep_missing[*]:-} $(command -v cmake >/dev/null || echo cmake) $(command -v cc >/dev/null || echo build-essential)"
    note "install them yourself, then run this again:  sudo apt install ${dep_missing[*]:-} cmake build-essential pkg-config"
    missing=1
else
    echo "fpga_toolchain.sh: building openFPGALoader $OFL_VERSION into $OFL_PREFIX" >&2
    mkdir -p "$DIR"
    tmp=$(mktemp -d "$DIR/.build.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    curl -fsSL --retry 3 -o "$tmp/ofl.tar.gz" "$OFL_URL"
    echo "$OFL_SHA256  $tmp/ofl.tar.gz" | sha256sum -c --quiet - >&2 || {
        echo "fpga_toolchain.sh: SHA-256 mismatch for openFPGALoader v$OFL_VERSION; not installed" >&2
        exit 1
    }
    tar -xzf "$tmp/ofl.tar.gz" -C "$tmp"
    cmake -S "$tmp/openFPGALoader-$OFL_VERSION" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OFL_PREFIX" >"$tmp/cmake.log" 2>&1 || { tail -20 "$tmp/cmake.log" >&2; exit 1; }
    cmake --build "$tmp/build" -j "$(nproc)" >"$tmp/build.log" 2>&1 || { tail -20 "$tmp/build.log" >&2; exit 1; }
    cmake --install "$tmp/build" >"$tmp/install.log" 2>&1 || { tail -20 "$tmp/install.log" >&2; exit 1; }
    ok "openFPGALoader $("$OFL_PREFIX/bin/openFPGALoader" --Version 2>&1 | head -1)"
fi

# ---- 3. permissions: reported, never changed --------------------------------
if grep -qsE 'idVendor}=="0403".*idProduct}=="6010"' /etc/udev/rules.d/*.rules /lib/udev/rules.d/*.rules; then
    ok "a udev rule for 0403:6010 (the BL616 presents itself as an FT2232)"
else
    miss "a udev rule for 0403:6010: see openFPGALoader's 99-openfpgaloader.rules ($OFL_PREFIX/share/openFPGALoader if built)"
    missing=1
fi
if id -nG | grep -qw plugdev; then ok "user in group plugdev"; else miss "user in group plugdev"; missing=1; fi

fi

# ---- manifest and PATH -------------------------------------------------------
if [[ $missing == 0 ]]; then
    {
        echo "date=$(date '+%Y-%m-%d %H:%M:%S %z')"
        echo "python=$("$VENV/bin/python" -c 'import sys; print(sys.version.split()[0])')"
        echo "requirements.lock_sha256=$lock_sha"
        py_clean "$VENV/bin/python" -I -m pip freeze --isolated --disable-pip-version-check | sed 's/^/pip: /'
        [[ $synthesis_only == 1 ]] || echo "openFPGALoader=$OFL_VERSION sha256=$OFL_SHA256"
    } > "$DIR/MANIFEST.txt"
    # yowasp keeps its compiled WebAssembly here instead of ~/.cache/YoWASP.
    echo "export PATH=\"$VENV/bin:$OFL_PREFIX/bin:\$PATH\" YOWASP_CACHE_DIR=\"$DIR/yowasp-cache\""
else
    echo "fpga_toolchain.sh: not ready (see MISSING above)" >&2
    exit 1
fi
