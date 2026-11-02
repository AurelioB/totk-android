#!/usr/bin/env bash
# Build the Mesa Turnip Vulkan driver used by the TOTK Android port.
#
# The checkout is always Mesa main at $BASE plus patches/*.patch; HEAD's tree
# is verified against $TIP_TREE so the compiled sources are exactly those of
# the upstream branch the shipped driver came from.
#
# Everything mutable lives under $TURNIP_WORK (default games/totk/build/turnip,
# gitignored): the Mesa clone, build dirs, packages and logs. Nothing is
# installed into the repository or onto the host.
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../../../.." && pwd)

WORK=${TURNIP_WORK:-$REPO/games/totk/build/turnip}
mkdir -p "$WORK"
WORK=$(CDPATH= cd -- "$WORK" && pwd)

MESA_REPO=${MESA_REPO:-https://gitlab.freedesktop.org/mesa/mesa.git}
# Mesa main commit the patch series is based on (2026-10-19).
BASE=f7c93a1352b578fbea6c249227d44df65b8f6a35
# Upstream branch head the shipped driver was built from: BASE + patches/.
TIP=3e8621cc0e95a9c106d71da3143773dfba68da99
# Tree of $TIP; the patch series must reproduce it byte for byte.
TIP_TREE=479b6b28724efc8a39db7b9103e8d7e793775529
# Pinned NDK; the driver ABI and codegen depend on it.
NDK_REVISION=28.2.13676358
# SHA-256 of the release driver.so this recipe reproduces.
EXPECTED_RELEASE_SHA256=9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK ${NDK_REVISION} installation}"
NDK=$ANDROID_NDK_HOME
PYTHON=${PYTHON:-python3}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 6)}
VARIANTS=${TURNIP_VARIANTS:-release relwithsymbols}

MESA=$WORK/mesa
BUILD_PREFIX=$WORK/build-
PACKAGES=$WORK/packages
LOGS=$WORK/logs
PATCHES=$HERE/patches

mkdir -p "$LOGS" "$WORK/tmp"
export TMPDIR="$WORK/tmp" LC_ALL=C TZ=UTC PYTHONHASHSEED=0
export PIP_CACHE_DIR="$WORK/.pip-cache" PKG_CONFIG_PATH=""
unset PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
START=$SECONDS

die() { echo "$*" >&2; exit 1; }

# ---------------------------------------------------------------- Mesa source
if [[ ! -e "$MESA/.git" ]]; then
    echo "Cloning Mesa into $MESA (blobless, base commit only)"
    git clone --filter=blob:none --no-checkout --single-branch --branch main \
        "$MESA_REPO" "$MESA"
    git -C "$MESA" fetch --depth=1 origin "$BASE"
    git -C "$MESA" switch -c totk/base "$BASE"
fi

tree=$(git -C "$MESA" rev-parse "HEAD^{tree}")
if [[ "$tree" != "$TIP_TREE" ]]; then
    # Only a pristine checkout of the base commit may be patched.
    head=$(git -C "$MESA" rev-parse HEAD)
    [[ "$head" == "$BASE" ]] \
        || die "Mesa HEAD is $head with tree $tree, expected $TIP_TREE.
Remove $MESA and re-run to get a fresh patched checkout."
    [[ -z "$(git -C "$MESA" status --porcelain)" ]] \
        || die "Mesa checkout has local changes; commit, stash or remove $MESA."
    echo "Applying $(ls "$PATCHES"/*.patch | wc -l) patches onto $BASE"
    GIT_COMMITTER_NAME="TOTK Turnip build" \
    GIT_COMMITTER_EMAIL="totk-turnip@localhost" \
        git -C "$MESA" am --3way "$PATCHES"/*.patch
    tree=$(git -C "$MESA" rev-parse "HEAD^{tree}")
    [[ "$tree" == "$TIP_TREE" ]] \
        || die "Patch series produced tree $tree, expected $TIP_TREE."
fi
[[ -z "$(git -C "$MESA" status --porcelain)" ]] \
    || die "Mesa checkout has local changes; commit, stash or remove $MESA."
echo "Mesa sources verified: $TIP_TREE"
# The patches are applied with `git am`, so local commit ids differ from the
# upstream branch while the tree is identical. Mesa embeds the first 10 hex
# digits of HEAD in its version string, so pin it to the upstream tip.
export MESA_GIT_SHA1_OVERRIDE=$TIP

# ------------------------------------------------------------------ NDK + host
BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
[[ -x "$BIN/aarch64-linux-android33-clang" ]] \
    || die "NDK not found at $NDK (set ANDROID_NDK_HOME)"
grep -q "Pkg.Revision = $NDK_REVISION" "$NDK/source.properties" 2>/dev/null \
    || die "This build pins NDK $NDK_REVISION; $NDK is something else."

# Pinned Ubuntu 26.04 (amd64) build tools. They are extracted into the work dir
# instead of installed on the host, so no root privileges are needed. Set
# TURNIP_TOOL_DEBS to a directory of these exact .deb files for an offline build.
TOOL_PACKAGES=(
    'bison=2:3.8.2+dfsg-1ubuntu0.26.04.1'
    'flex=2.6.4-8.2build2'
    'm4=1.4.21-1'
    'glslang-tools=16.2.0-2'
    'spirv-tools=2026.1-1'
)
TOOLS=$WORK/tools
have_tools() {
    local tool
    for tool in bison flex m4 glslangValidator; do
        command -v "$tool" >/dev/null || return 1
    done
}
export PATH="$TOOLS/usr/bin:$PATH"
if ! have_tools; then
    mkdir -p "$TOOLS" "$WORK/tool-packages"
    if [[ -n "${TURNIP_TOOL_DEBS:-}" ]]; then
        debs=$TURNIP_TOOL_DEBS
    elif command -v apt-get >/dev/null && command -v dpkg-deb >/dev/null; then
        debs=$WORK/tool-packages
        (cd "$debs" && apt-get download "${TOOL_PACKAGES[@]}")
    else
        debs=
    fi
    if [[ -n "$debs" ]]; then
        for package in "$debs"/*.deb; do
            [[ -e "$package" ]] || die "No .deb packages in $debs"
            dpkg-deb -x "$package" "$TOOLS"
        done
    fi
    # Fall back to host tools only when the pinned set is unavailable.
    if ! have_tools; then
        for tool in bison flex m4 glslangValidator; do
            PATH=/usr/bin:/bin command -v "$tool" >/dev/null \
                || die "$tool is missing: the pinned .deb download failed; set TURNIP_TOOL_DEBS to a directory of $WORK/tool-packages-style .deb files"
        done
        export PATH="/usr/bin:/bin:$PATH"
        echo "warning: using host bison/flex/m4/glslang; the driver hash may differ" >&2
    fi
fi
if [[ -d "$TOOLS/usr/share/bison" ]]; then
    export BISON_PKGDATADIR="$TOOLS/usr/share/bison"
fi
export M4="$(command -v m4)"

# Pinned Meson/ninja/python deps; the venv is part of the work dir.
if [[ ! -x "$WORK/.venv/bin/meson" ]]; then
    "$PYTHON" -m venv "$WORK/.venv"
    "$WORK/.venv/bin/pip" install -r "$HERE/requirements.txt"
fi
export PATH="$WORK/.venv/bin:$PATH"
MESON=$WORK/.venv/bin/meson
export MESON

export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$MESA" show -s --format=%ct "$BASE")}

# ------------------------------------------------------------- cross toolchain
# Restrict cross dependencies to NDK libraries, never host /usr/lib.
mkdir -p "$WORK/pkgconfig"
cat > "$WORK/pkgconfig/zlib.pc" <<EOF
Name: zlib
Description: Android NDK platform zlib
Version: 1.3.0.1
Libs: -lz
Cflags:
EOF
CROSS="$WORK/android-aarch64-api33.ini"
PKG_CONFIG_BIN=${PKG_CONFIG_BIN:-/usr/bin/pkg-config}
cat > "$CROSS.new" <<EOF
# Generated by build.sh for NDK r28c / Android API 33.
[binaries]
c = '$BIN/aarch64-linux-android33-clang'
cpp = '$BIN/aarch64-linux-android33-clang++'
ar = '$BIN/llvm-ar'
strip = '$BIN/llvm-strip'
c_ld = 'lld'
cpp_ld = 'lld'
pkg-config = '$PKG_CONFIG_BIN'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$WORK/pkgconfig']

[built-in options]
# Collapse the absolute work dir so __FILE__ and debug paths are
# reproducible regardless of where the build runs.
c_args = ['-fno-omit-frame-pointer', '-ffile-prefix-map=$WORK=/turnip']
cpp_args = ['-fno-omit-frame-pointer', '-ffile-prefix-map=$WORK=/turnip']
cpp_link_args = ['-static-libstdc++']
EOF
if ! cmp -s "$CROSS.new" "$CROSS"; then mv "$CROSS.new" "$CROSS"; else rm "$CROSS.new"; fi

OPTIONS=(
    --cross-file "$CROSS" --buildtype release --wrap-mode nofallback
    -Dplatforms=android -Dplatform-sdk-version=33 -Dandroid-stub=true
    -Dandroid-libbacktrace=disabled -Dandroid-libperfetto=disabled
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl
    -Degl=disabled -Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=disabled
    -Dzstd=disabled -Dzlib=enabled -Dvalgrind=disabled -Dlibunwind=disabled
    -Dbuild-tests=false -Dtools= -Dspirv-tools=disabled
    -Db_lto=false -Db_ndebug=true -Doptimization=3 -Dstrip=false
)

# ---------------------------------------------------------------------- build
for variant in $VARIANTS; do
    case "$variant" in
        release)         DEBUG=false; TYPE=release ;;
        relwithsymbols)  DEBUG=true;  TYPE=custom  ;; # release overrides -Ddebug on setup
        *) die "unknown variant $variant" ;;
    esac
    BUILD="$BUILD_PREFIX$variant"
    VSTART=$SECONDS
    if [[ -f "$BUILD/meson-private/coredata.dat" ]]; then
        "$MESON" setup --clearcache --reconfigure "$BUILD" "$MESA" "${OPTIONS[@]}" \
            --buildtype "$TYPE" -Ddebug="$DEBUG" 2>&1 | tee "$LOGS/$variant-configure.log"
    else
        "$MESON" setup "$BUILD" "$MESA" "${OPTIONS[@]}" \
            --buildtype "$TYPE" -Ddebug="$DEBUG" 2>&1 | tee "$LOGS/$variant-configure.log"
    fi
    # Build/install the driver and ICD metadata; no stub DSOs are installed.
    ninja -C "$BUILD" -j "$JOBS" \
        src/freedreno/vulkan/libvulkan_freedreno.so \
        src/freedreno/vulkan/freedreno_icd.armv8-a.json 2>&1 | tee "$LOGS/$variant-build.log"
    # Meson's install step removes build-tree RUNPATHs pointing at platform stubs.
    "$MESON" install -C "$BUILD" --no-rebuild --destdir "$BUILD/stage" \
        2>&1 | tee "$LOGS/$variant-install.log"
    OUT="$PACKAGES/$variant"
    mkdir -p "$OUT"
    cp "$BUILD/stage/usr/local/lib/libvulkan_freedreno.so" "$OUT/vulkan.freedreno.so"
    cp "$OUT/vulkan.freedreno.so" "$OUT/driver.so"
    if [[ "$variant" == relwithsymbols ]]; then
        "$BIN/llvm-objcopy" --only-keep-debug "$OUT/driver.so" "$OUT/driver.so.debug"
        # Preserve full symbols in driver.so for profilers; debug-only copy is also provided.
        "$BIN/llvm-objcopy" --add-gnu-debuglink="$OUT/driver.so.debug" "$OUT/driver.so"
    else
        "$BIN/llvm-strip" --strip-all "$OUT/driver.so" "$OUT/vulkan.freedreno.so"
    fi
    "$WORK/.venv/bin/python" "$HERE/package.py" "$WORK" "$variant" "$((SECONDS-VSTART))" \
        | tee "$LOGS/$variant-package.log"
done
printf '%s\n' "$((SECONDS-START))" > "$LOGS/build-total-seconds.txt"

# -------------------------------------------------------------------- results
for variant in $VARIANTS; do
    echo "$variant: $PACKAGES/$variant/driver.so"
done
release_sha=$(sha256sum "$PACKAGES/release/driver.so" 2>/dev/null | cut -d' ' -f1 || true)
if [[ -n "$release_sha" ]]; then
    echo "release driver.so sha256: $release_sha"
    if [[ "$release_sha" == "$EXPECTED_RELEASE_SHA256" ]]; then
        echo "matches the recorded release driver"
    else
        echo "warning: differs from the recorded release driver $EXPECTED_RELEASE_SHA256" >&2
        echo "         (toolchain/NDK/tool versions differ from the reference build)" >&2
    fi
fi
echo "Work dir: $WORK; logs: $LOGS"
