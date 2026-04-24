# SeedCrackerZ build
#
# `make` downloads the cubiomes library and the bundled pthread fallback
# (no git required), then compiles seedcrackerz into a single binary.
# Re-running `make` is a no-op once everything is in place.
#
#   make            build
#   make clean      remove the binary
#   make distclean  remove the binary, cubiomes/, and compat/

# Prefer `cc`; fall back to `clang` (e.g. iOS a-Shell, where `cc` is missing).
CC      ?= $(shell command -v cc 2>/dev/null || command -v clang 2>/dev/null || echo cc)

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

seedcrackerz: deps-stamp seedcrackerz.c
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Marker file: present iff every cubiomes source AND compat/pthread.h is on disk.
deps-stamp:
	@echo "Fetching cubiomes library..."
	@mkdir -p cubiomes/tables compat
	@for f in $(CUBIOMES_FILES); do \
	    if [ ! -s cubiomes/$$f ]; then \
	        echo "  cubiomes/$$f"; \
	        curl -fsSL -o cubiomes/$$f "$(CUBIOMES_BASE)/$$f" || exit 1; \
	    fi; \
	done
	@for f in $(CUBIOMES_TABLES); do \
	    if [ ! -s cubiomes/tables/$$f ]; then \
	        echo "  cubiomes/tables/$$f"; \
	        curl -fsSL -o cubiomes/tables/$$f "$(CUBIOMES_BASE)/tables/$$f" || exit 1; \
	    fi; \
	done
	@if [ ! -s compat/pthread.h ]; then \
	    echo "  compat/pthread.h"; \
	    curl -fsSL -o compat/pthread.h "$(SEEDCRACKERZ_BASE)/compat/pthread.h" || exit 1; \
	fi
	@echo "dependencies ready."
	@touch deps-stamp

clean:
	rm -f seedcrackerz deps-stamp

distclean: clean
	rm -rf cubiomes compat

.PHONY: all clean distclean
