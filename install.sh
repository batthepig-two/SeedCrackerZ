#!/bin/sh
# SeedCrackerZ installer — no git, no make required
# Works on: a-Shell (iOS), iSH (iOS), Termux (Android), macOS, Linux
# Needs: curl + clang  (installer will try to fetch them if missing)

set -e

REPO_RAW="https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main"
CUBIOMES_RAW="https://raw.githubusercontent.com/cubitect/cubiomes/master"
DEST="SeedCrackerZ"

echo "=== SeedCrackerZ Installer ==="
echo ""

# ── Platform detection + dependency bootstrap ──────────────────────────────
OS="$(uname -s 2>/dev/null || echo unknown)"
PLATFORM="unknown"

# iSH (Alpine Linux on iOS) — /proc/ish exists
if [ -f /proc/ish ]; then
    PLATFORM="ish"
# Termux (Android) — PREFIX points inside the Termux tree
elif [ -n "$PREFIX" ] && echo "$PREFIX" | grep -q termux; then
    PLATFORM="termux"
# a-Shell (iOS) — TERM_PROGRAM is a-Shell, or clang is wasm32-targeted
elif [ "$TERM_PROGRAM" = "a-Shell" ] || (clang --version 2>/dev/null | grep -q wasm); then
    PLATFORM="ashell"
elif [ "$OS" = "Darwin" ]; then
    PLATFORM="macos"
elif [ "$OS" = "Linux" ]; then
    PLATFORM="linux"
fi

echo "Detected platform: $PLATFORM"
echo ""

# Install missing build tools where we can do it automatically
if [ "$PLATFORM" = "ish" ]; then
    if ! command -v clang >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
        echo "Installing build tools via apk..."
        apk add --no-cache clang curl
    fi
elif [ "$PLATFORM" = "termux" ]; then
    if ! command -v clang >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
        echo "Installing build tools via pkg..."
        pkg install -y clang curl
    fi
fi

# Verify we have what we need
if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl not found. Install it and re-run this script." >&2
    exit 1
fi
CCBIN=""
for candidate in clang cc gcc; do
    if command -v "$candidate" >/dev/null 2>&1; then
        CCBIN="$candidate"
        break
    fi
done
if [ -z "$CCBIN" ]; then
    echo "error: no C compiler found (tried clang, cc, gcc)." >&2
    if [ "$PLATFORM" = "linux" ] || [ "$PLATFORM" = "ish" ]; then
        echo "       Run: sudo apt install clang  (or: apk add clang)" >&2
    elif [ "$PLATFORM" = "macos" ]; then
        echo "       Run: xcode-select --install" >&2
    fi
    exit 1
fi

# ── Set up directories ─────────────────────────────────────────────────────
rm -rf "$DEST"
mkdir -p "$DEST/cubiomes/tables"
mkdir -p "$DEST/compat"
cd "$DEST"

# ── 1/3  Source files ──────────────────────────────────────────────────────
echo "[1/3] Downloading SeedCrackerZ source..."
curl -fsSL "$REPO_RAW/seedcrackerz.c" -o seedcrackerz.c
curl -fsSL "$REPO_RAW/compat/pthread.h" -o compat/pthread.h

# ── 2/3  cubiomes ──────────────────────────────────────────────────────────
echo "[2/3] Downloading cubiomes..."
for f in generator.c generator.h finders.c finders.h biomes.c biomes.h \
          noise.c noise.h util.c util.h layers.c layers.h rng.h \
          biomenoise.c biomenoise.h quadbase.c quadbase.h; do
    curl -fsSL "$CUBIOMES_RAW/$f" -o "cubiomes/$f"
done
for t in btree18.h btree19.h btree192.h btree20.h btree21wd.h; do
    curl -fsSL "$CUBIOMES_RAW/tables/$t" -o "cubiomes/tables/$t"
done

# ── 3/3  Compile ───────────────────────────────────────────────────────────
echo "[3/3] Compiling..."

CFLAGS="-O3 -std=c11 -Icubiomes -idirafter compat -Wall -Wno-unused-function -Wno-macro-redefined"

# a-Shell (wasm32-wasi) needs an explicit stack size; harmless on other targets
EXTRA=""
if [ "$PLATFORM" = "ashell" ]; then
    EXTRA="-Wl,-z,stack-size=8388608"
fi

$CCBIN $CFLAGS $EXTRA \
    seedcrackerz.c \
    cubiomes/generator.c cubiomes/finders.c cubiomes/biomes.c \
    cubiomes/noise.c cubiomes/util.c cubiomes/layers.c \
    cubiomes/biomenoise.c cubiomes/quadbase.c \
    -lm -o seedcrackerz

echo ""
echo "Done! Run with:"
echo "  cd $DEST && ./seedcrackerz"
