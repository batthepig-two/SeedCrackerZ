CC      = cc
CFLAGS  = -O3 -std=c11 -Icubiomes -Wall -Wno-unused-function
LDFLAGS = -lm

CUBIOMES_SRC = cubiomes/generator.c cubiomes/finders.c cubiomes/biomes.c \
               cubiomes/noise.c cubiomes/util.c cubiomes/layers.c        \
               cubiomes/biomenoise.c cubiomes/quadbase.c

SRCS = seedcrackerz.c $(CUBIOMES_SRC)

all: seedcrackerz

seedcrackerz: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

cubiomes/generator.c:
	@echo "Fetching cubiomes library..."
	@if command -v git >/dev/null 2>&1; then \
		git clone --depth=1 https://github.com/cubitect/cubiomes.git cubiomes; \
	else \
		mkdir -p cubiomes && \
		for f in generator.c generator.h finders.c finders.h biomes.c biomes.h \
		         noise.c noise.h util.c util.h layers.c layers.h rng.h         \
		         biomenoise.c biomenoise.h quadbase.c quadbase.h; do            \
		    curl -s -o cubiomes/$$f                                             \
		      "https://raw.githubusercontent.com/cubitect/cubiomes/master/$$f"; \
		done; \
	fi
	@echo "cubiomes ready."

$(SRCS): cubiomes/generator.c

clean:
	rm -f seedcrackerz

distclean: clean
	rm -rf cubiomes

.PHONY: all clean distclean
