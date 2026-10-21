#!/bin/sh
# Build the shader translation service (totk_xlate --service): vendor/ryujinx + patches + Service.cs,
# ahead of time into one native executable. Playing needs no .NET runtime; publishing needs the .NET 10 SDK
# and network access the first time (NuGet packages and the NativeAOT compiler).
# usage: publish_native.sh [dotnet] [--out DIR]
#          -> prebuilt/macos-arm64, linux-x64 or linux-arm64/totk_xlate (+ .sha256)
#        publish_native.sh --android --ndk /path/to/ndk [--api 33] [--dotnet /path/to/dotnet] [--out DIR]
#          -> prebuilt/android-arm64/totk_xlate (+ .sha256), a PIE executable, not a shared library
# Host Linux publishing also needs clang and zlib (Fedora: dnf install dotnet-sdk-10.0 clang zlib-devel).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
DOTNET=${DOTNET:-dotnet}
ANDROID=0
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
API=33
OUT=
fail() { echo "publish_native.sh: $*" >&2; exit 2; }
while [ "$#" -gt 0 ]; do
  case "$1" in
    --android) ANDROID=1; shift ;;
    --ndk|--api|--out|--dotnet)
      [ "$#" -ge 2 ] || fail "$1 needs a value"
      case "$1" in
        --ndk) NDK=$2 ;;
        --api) API=$2 ;;
        --out) OUT=$2 ;;
        --dotnet) DOTNET=$2 ;;
      esac
      shift 2 ;;
    -*) fail "unknown option $1" ;;
    *) DOTNET=$1; shift ;;   # legacy positional dotnet executable
  esac
done
set --
if [ "$ANDROID" -eq 1 ]; then
  [ -n "$NDK" ] || fail "--android needs --ndk PATH (or ANDROID_NDK_HOME)"
  case "$API" in ''|*[!0-9]*) fail "--api must be an integer >=33" ;; esac
  [ "$API" -ge 33 ] || fail "--android needs API 33 or newer"
  case "$(uname -s)" in
    Linux) HOST=linux-x86_64 ;;
    Darwin) HOST=darwin-x86_64 ;;
    *) fail "unsupported Android publishing host $(uname -s)" ;;
  esac
  NDKBIN=$NDK/toolchains/llvm/prebuilt/$HOST/bin
  [ -d "$NDKBIN" ] || fail "no NDK toolchain at $NDKBIN"
  NDKBIN=$(CDPATH= cd -- "$NDKBIN" && pwd)
  CC=$NDKBIN/aarch64-linux-android$API-clang
  [ -x "$CC" ] || fail "NDK compiler missing: $CC"
  [ -x "$NDKBIN/llvm-objcopy" ] || fail "NDK llvm-objcopy missing: $NDKBIN/llvm-objcopy"
  command -v python3 >/dev/null 2>&1 || fail "--android needs python3 for the native-crypto publication check"
  export PATH="$NDKBIN:$PATH"
  RID=linux-bionic-arm64
  PLATFORM=android-arm64
  set -- -p:PublishAotUsingRuntimePack=true "-p:CppCompilerAndLinker=$CC"
else
  case "$(uname -s)-$(uname -m)" in
    Darwin-*) RID=osx-arm64; PLATFORM=macos-arm64 ;;
    Linux-x86_64) RID=linux-x64; PLATFORM=linux-x64 ;;
    Linux-aarch64) RID=linux-arm64; PLATFORM=linux-arm64 ;;
    *) fail "unsupported host $(uname -s)-$(uname -m)" ;;
  esac
fi
command -v "$DOTNET" >/dev/null 2>&1 || fail "$DOTNET not found; publishing needs the .NET 10 SDK"
SDK_VERSION=$("$DOTNET" --version) || fail "cannot run $DOTNET; publishing needs the .NET 10 SDK"
case "$SDK_VERSION" in 10.*) ;; *) fail "needs the .NET 10 SDK (selected $SDK_VERSION)" ;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$REPO/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
if [ "$ANDROID" -eq 1 ]; then
  # NDK r28+ already defaults to 16 KiB pages. Keep older supported NDKs compatible too;
  # NativeAOT's own bionic alignment rule only covers shared libraries, not our executable.
  printf '%s\n' '<Project><ItemGroup><LinkerArg Include="-Wl,-z,max-page-size=16384" /></ItemGroup></Project>' \
    > "$WORK/xlate/Directory.Build.targets"
fi
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" -c Release -r "$RID" --self-contained true \
  -p:PublishAot=true -p:InvariantGlobalization=true "$@" -p:RyuSrc="$WORK/ryujinx-src" -o "$WORK/out"
if [ "$ANDROID" -eq 1 ]; then
  python3 "$HERE/check_android_crypto.py" "$WORK/out/totk_xlate"
fi
OUT=${OUT:-$REPO/games/totk/prebuilt/$PLATFORM}
mkdir -p "$OUT"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
chmod 755 "$OUT/totk_xlate"
if command -v shasum >/dev/null; then shasum -a 256 "$OUT/totk_xlate"; else sha256sum "$OUT/totk_xlate"; fi | awk '{print $1}' > "$OUT/totk_xlate.sha256"
echo "built $OUT/totk_xlate (sha256 $(cat "$OUT/totk_xlate.sha256"))"
