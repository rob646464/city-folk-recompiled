#!/bin/bash
# Builds and packages the Linux release (docs/SETUP.md "Packages", same as LM's docs/LINUX.md):
#   tools/package_linux.sh <LLVM bin dir> [out dir]
# <LLVM bin dir>: bin/ of the official LLVM release for Linux x86_64 (LLVM-<ver>-Linux-X64),
# whose clang and ld.lld are bundled. Produces <out>/CityFolkRecompiled-linux-x86_64/ plus
# .tar.gz and .zip of it. No game data is involved (the cfrecomp dev build is skipped: it needs
# the user's disc).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LLVM_BIN=${1:?usage: package_linux.sh <LLVM bin dir> [out dir]}
OUT=${2:-$ROOT/out}
NAME=CityFolkRecompiled-linux-x86_64
BUILD=${CF_LINUX_BUILD:-$ROOT/build/linux-clang}
EXE=CityFolkRecomp

[ -x "$LLVM_BIN/clang" ] && [ -e "$LLVM_BIN/ld.lld" ] || { echo "no clang/ld.lld in $LLVM_BIN" >&2; exit 1; }

cd "$ROOT"
# CF_DISC pointing nowhere: never the dev build, whatever a cache or checkout holds.
cmake --preset linux-clang -B "$BUILD" -DCF_LLVM_DIR="$LLVM_BIN" -DCF_DISC=/nonexistent > /dev/null
cmake --build "$BUILD" --target $EXE cf_libxml2_stub

rm -rf "${OUT:?}/$NAME"
mkdir -p "$OUT"
cmake --install "$BUILD" --prefix "$OUT/$NAME" --strip
PKG=$OUT/$NAME
for f in $EXE CityFolkRecompiled.sh README.txt llvm/bin/clang llvm/bin/ld.lld llvm/lib/libxml2.so.2 llvm/LLVM-LICENSE.TXT; do
    [ -e "$PKG/$f" ] || { echo "package is missing $f" >&2; exit 1; }
done
[ -e "$PKG/pipelines.bin" ] || echo "warning: no pipelines.bin (data/pipelines.bin missing): first starts compile every shader in play" >&2

# Sanity checks: what the package needs from the system.
echo "== $NAME"
(cd "$PKG" && find . -type f -printf '%m %10s %p\n' | sort -k3)
echo "== required glibc / libstdc++ versions"
for b in "$PKG/$EXE" "$PKG/llvm/bin/clang" "$PKG/llvm/bin/ld.lld"; do
    printf '%s: ' "${b#$OUT/}"
    objdump -T "$b" | grep -o 'GLIBC_[0-9.]*\|GLIBCXX_[0-9.]*' | sort -uV | awk -F_ '{m[$1]=$0} END {printf "%s %s\n", m["GLIBC"], m["GLIBCXX"]}'
    readelf -d "$b" | grep NEEDED | sed 's/.*\[\(.*\)\]/  needs \1/' | tr '\n' ' '; echo
done
"$PKG/llvm/bin/clang" --version | head -1
"$PKG/llvm/bin/ld.lld" --version
# The release must load on glibc 2.34 (LM docs/LINUX.md "Portable executable"): fail on newer
# symbol versions (add a shim to src/port/glibc_compat.c).
for b in "$PKG/$EXE" "$PKG/llvm/bin/clang" "$PKG/llvm/bin/ld.lld"; do
    if objdump -T "$b" | grep -E 'GLIBC_2\.(3[5-9]|[4-9][0-9])'; then
        echo "${b#$OUT/} needs glibc newer than 2.34 (symbols above)" >&2
        exit 1
    fi
done
# A name the executable both exports and imports (a glibc shim that calls the original through
# the PLT) binds to itself at run time: the loader lets the executable's unversioned definition
# satisfy the versioned import, so the first call loops forever (LM issues #2/#11, hypotf).
# Shims must be hidden (src/port/glibc_compat.c).
selfbound=$(objdump -T "$PKG/$EXE" | awk '/^[0-9a-f]+ / { if (/\*UND\*/) u[$NF] = 1; else d[$NF] = 1 } END { for (n in u) if (n in d) print n }')
if [ -n "$selfbound" ]; then
    echo "$EXE exports names it also imports (they would call themselves): $selfbound" >&2
    exit 1
fi

# The bundled toolchain builds a game module the way setup does (src/setup/module_builder.cpp
# kCompileFlags/kLinkFlags: freestanding, no system headers or libraries) and the loader opens it.
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
FLAGS=(--target=x86_64-pc-linux-gnu -std=c++20 -O2 -fPIC -fvisibility=hidden -ffreestanding -nostdinc
       -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-math-errno -fno-stack-protector
       -funwind-tables -Wno-unused-label -Wno-unused-variable -Wno-missing-field-initializers "-I$T/include")
"$PKG/llvm/bin/clang" "${FLAGS[@]}" -c src/recomp/module_rt.cpp -o "$T/rt.o"
"$PKG/llvm/bin/clang" "${FLAGS[@]}" -c "$T/table.cpp" -o "$T/table.o"
"$PKG/llvm/bin/ld.lld" -shared -nostdlib --eh-frame-hdr -Bsymbolic -z defs -z noexecstack -z now -O1 \
    --hash-style=both -soname cf_game_module.so -o "$T/module.so" "$T/rt.o" "$T/table.o"
python3 - "$T/module.so" <<'EOF'
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)
assert lib.cf_module_init, "cf_module_init missing"
print("module loads, cf_module_init exported")
EOF

cd "$OUT"
rm -f "$NAME.tar.gz" "$NAME.zip"
tar -czf "$NAME.tar.gz" --owner=0 --group=0 "$NAME"
zip -qry "$NAME.zip" "$NAME"
ls -la "$NAME.tar.gz" "$NAME.zip"
du -sh "$NAME"
