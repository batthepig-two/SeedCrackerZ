# SeedCrackerZ build
#
# `make` downloads the cubiomes library and the bundled pthread fallback
# (no git required), then compiles seedcrackerz into a single binary.
# The download step is idempotent: missing files are re-fetched on every
# run, so a partial or stale checkout self-heals.
#
#   make            build
#   make clean      remove the binary
#   make distclean  remove the binary, cubiomes/, and compat/
#
# This Makefile is written to be portable across GNU make (Linux, macOS
# with Homebrew, WSL, Termux) and BSD make (default `make` on iOS
# a-Shell, FreeBSD, NetBSD). It avoids GNU-only constructs:
#   * No `target: deps | order-only` syntax  -- bmake reads `|` as a target.
#   * No `$(shell ...)` for compiler detection -- handled in the recipe.

# `-idirafter compat` puts the local pthread shim *after* the system search
# path, so a real <pthread.h> is always preferred when one exists.
CFLAGS  = -O3 -std=c11 -Icubiomes -idirafter compat \
          -Wall -Wno-unused-function -Wno-macro-redefined
LDFLAGS = -lm

CUBIOMES_SRC = cubiomes/generator.c cubiomes/finders.c cubiomes/biomes.c \
               cubiomes/noise.c cubiomes/util.c cubiomes/layers.c        \
               cubiomes/biomenoise.c cubiomes/quadbase.c

SRCS = seedcrackerz.c $(CUBIOMES_SRC)

CUBIOMES_FILES  = generator.c generator.h finders.c finders.h biomes.c biomes.h \
                  noise.c noise.h util.c util.h layers.c layers.h rng.h         \
                  biomenoise.c biomenoise.h quadbase.c quadbase.h
CUBIOMES_TABLES = btree18.h btree19.h btree192.h btree20.h btree21wd.h
CUBIOMES_BASE   = https://raw.githubusercontent.com/cubitect/cubiomes/master

# Where to fetch the pthread fallback shim from when it isn't already present
# (e.g. when the user only ran `curl -O` for seedcrackerz.c and the Makefile).
SEEDCRACKERZ_BASE = https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main

all: seedcrackerz

# Recursive `$(MAKE) deps` is portable -- works under both GNU make and
# bmake -- and avoids the order-only-prerequisite syntax that bmake doesn't
# understand. `deps` is .PHONY so it always runs (cheap when nothing is
# missing), and seedcrackerz only relinks when seedcrackerz.c actually
# changes.
#
# Compiler detection is done at recipe time, not at parse time, so it works
# on bmake (which has no `$(shell ...)` function) as well as GNU make.
# Order of preference: $$CC from environment, then $(CC) macro, then `cc`,
# then `clang` (covers iOS a-Shell, which only ships clang).
seedcrackerz: seedcrackerz.c
	@$(MAKE) deps
	@CCBIN="$${CC:-}"; \
	if [ -z "$$CCBIN" ] || ! command -v "$$CCBIN" >/dev/null 2>&1; then \
	    CCBIN="$$(command -v $(CC) 2>/dev/null || command -v cc 2>/dev/null || command -v clang 2>/dev/null)"; \
	fi; \
	if [ -z "$$CCBIN" ]; then \
	    echo "error: no C compiler found. Install clang or cc, or set CC=..." >&2; \
	    exit 1; \
	fi; \
	echo "$$CCBIN $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)"; \
	$$CCBIN $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Always-run dependency check. Each download is gated on `if [ ! -s file ]`,
# so re-runs are essentially free when nothing is missing.
deps:
	@mkdir -p cubiomes/tables compat
	@announced=0; \
	for f in $(CUBIOMES_FILES); do \
	    if [ ! -s cubiomes/$$f ]; then \
	        if [ $$announced -eq 0 ]; then echo "Fetching dependencies..."; announced=1; fi; \
	        echo "  cubiomes/$$f"; \
	        curl -fsSL -o cubiomes/$$f "$(CUBIOMES_BASE)/$$f" || exit 1; \
	    fi; \
	done; \
	for f in $(CUBIOMES_TABLES); do \
	    if [ ! -s cubiomes/tables/$$f ]; then \
	        if [ $$announced -eq 0 ]; then echo "Fetching dependencies..."; announced=1; fi; \
	        echo "  cubiomes/tables/$$f"; \
	        curl -fsSL -o cubiomes/tables/$$f "$(CUBIOMES_BASE)/tables/$$f" || exit 1; \
	    fi; \
	done; \
	if [ ! -s compat/pthread.h ]; then \
	    if [ $$announced -eq 0 ]; then echo "Fetching dependencies..."; announced=1; fi; \
	    echo "  compat/pthread.h"; \
	    curl -fsSL -o compat/pthread.h "$(SEEDCRACKERZ_BASE)/compat/pthread.h" || exit 1; \
	fi; \
	if [ $$announced -eq 1 ]; then echo "dependencies ready."; fi

clean:
	rm -f seedcrackerz cubiomes-stamp deps-stamp

distclean: clean
	rm -rf cubiomes compat

.PHONY: all clean distclean deps
