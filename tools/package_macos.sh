#!/bin/bash
# Builds and packages the macOS release (docs/SETUP.md "Packages"; LM docs/MACOS.md):
#   tools/package_macos.sh <LLVM bin dir> [out dir]
# <LLVM bin dir>: bin/ of the official LLVM release for macOS arm64 (LLVM-<ver>-macOS-ARM64),
# whose clang and ld64.lld are bundled (Homebrew's need libLLVM next to them). Produces
# <out>/CityFolkRecompiled-macos-arm64/ plus .tar.gz and .zip of it. No game data is involved.
# Fails when anything in the package links a library outside macOS itself, or when the bundled
# toolchain cannot build and load a game module. Run by GitHub Actions only (nobody here has a Mac).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LLVM_BIN=${1:?usage: package_macos.sh <LLVM bin dir> [out dir]}
OUT=${2:-$ROOT/out}
NAME=CityFolkRecompiled-macos-arm64
BUILD=$ROOT/build/macos-clang
EXE=CityFolkRecomp

[ -x "$LLVM_BIN/clang" ] && [ -e "$LLVM_BIN/ld64.lld" ] || { echo "no clang/ld64.lld in $LLVM_BIN" >&2; exit 1; }

cd "$ROOT"
cmake --preset macos-clang -DCF_LLVM_DIR="$LLVM_BIN" -DCF_DISC=/nonexistent > /dev/null
cmake --build "$BUILD" -j "${CF_BUILD_JOBS:-6}" --target $EXE

rm -rf "${OUT:?}/$NAME"
mkdir -p "$OUT"
cmake --install "$BUILD" --prefix "$OUT/$NAME" --strip
PKG=$OUT/$NAME
for f in $EXE README.txt llvm/bin/clang llvm/bin/ld64.lld llvm/LLVM-LICENSE.TXT; do
    [ -e "$PKG/$f" ] || { echo "package is missing $f" >&2; exit 1; }
done
[ -e "$PKG/pipelines.bin" ] || echo "warning: no pipelines.bin (data/pipelines.bin missing)" >&2

echo "== $NAME"
(cd "$PKG" && find . -type f -exec stat -f '%Lp %10z %N' {} + | sort -k3)

# Only macOS's own libraries: anything from Homebrew or @rpath would be missing on a player's Mac.
echo "== libraries"
bad=0
for b in "$PKG/$EXE" "$PKG/llvm/bin/clang" "$PKG/llvm/bin/ld64.lld"; do
    echo "${b#$OUT/}:"
    otool -L "$b" | tail -n +2 | awk '{print "  " $1}'
    if otool -L "$b" | tail -n +2 | awk '{print $1}' | grep -vE '^(/System/Library/|/usr/lib/)'; then
        echo "  ^ not part of macOS" >&2
        bad=1
    fi
    codesign -v "$b" || { echo "  ^ invalid code signature" >&2; bad=1; }
done
[ $bad = 0 ] || exit 1
"$PKG/llvm/bin/clang" --version | head -1
"$PKG/llvm/bin/ld64.lld" --version

# The bundled toolchain builds a game module the way setup does (src/setup/module_builder.cpp:
# freestanding, no SDK) and macOS loads it.
echo "== module smoke test"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/include/recomp"
cp src/recomp/ctx.h src/recomp/module_abi.h "$T/include/recomp/"
cat > "$T/table.cpp" <<'EOF'
#include "recomp/module_abi.h"
namespace recomp {
void f_80003100(Ctx& c) {
    volatile unsigned char big[100000];
    for (int i = 0; i < 100000; i += 4096) big[i] = (unsigned char)c.r[3];
    c.r[3] = big[c.r[4] % 100000];
}
extern const FuncEntry kFuncTable[] = {{0x80003100u, &f_80003100}};
extern const SymEntry kSymTable[] = {{"smoke", 0x80003100u}};
extern const FuncEntry kOrigTable[] = {{0, nullptr}};
extern const GameModule kGameModule = {kModuleAbiVersion, "RUUE01", kFuncTable, 1u, kSymTable, 1u, kOrigTable, 0u};
}
EOF
FLAGS=(--target=arm64-apple-macos13.0 -std=c++20 -O2 -fPIC -fvisibility=hidden -ffreestanding -nostdinc
       -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-math-errno -fno-stack-protector -ffp-contract=off
       -funwind-tables -Wno-unused-label -Wno-unused-variable -Wno-missing-field-initializers "-I$T/include")
"$PKG/llvm/bin/clang" "${FLAGS[@]}" -c src/recomp/module_rt.cpp -o "$T/rt.o"
"$PKG/llvm/bin/clang" "${FLAGS[@]}" -c "$T/table.cpp" -o "$T/table.o"
"$PKG/llvm/bin/ld64.lld" -dylib -arch arm64 -platform_version macos 13.0 13.0 -install_name @rpath/cf_game_module.dylib \
    -undefined error -dead_strip -O1 -o "$T/module.dylib" "$T/rt.o" "$T/table.o"
python3 - "$T/module.dylib" <<'EOF'
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)
assert lib.cf_module_init, "cf_module_init missing"
print("module loads, cf_module_init exported")
EOF

cd "$OUT"
rm -f "$NAME.tar.gz" "$NAME.zip"
tar -czf "$NAME.tar.gz" "$NAME"
ditto -c -k --keepParent "$NAME" "$NAME.zip"
ls -la "$NAME.tar.gz" "$NAME.zip"
du -sh "$NAME"
