/*
 * seedcrackerz.c  —  Java Edition world seed cracker for any POSIX terminal.
 *
 * Provide structure locations, biome observations, slime-chunk results,
 * buried-treasure chunks, and/or your world-spawn point — then let
 * SeedCrackerZ search a seed range to find which world seed(s) produce
 * exactly that world.
 *
 * Powered by the cubiomes library (https://github.com/cubitect/cubiomes)
 * which implements Mojang's exact world-generation algorithms in C.
 *
 * Build:   make             (downloads cubiomes automatically, then compiles)
 * Run:     ./seedcrackerz
 *
 * ── Architecture ──────────────────────────────────────────────────────────
 * Java's RNG is internally 48-bit. Structure placement, slime chunks and
 * buried-treasure presence therefore depend ONLY on the lower 48 bits of
 * the world seed. Biome generation and world-spawn placement use the full
 * 64 bits.
 *
 * The search is split into two phases accordingly:
 *
 *   Phase A (lower 48):  iterate candidate lower-48 values, keep only those
 *                        that satisfy every structure, slime and buried-
 *                        treasure constraint.
 *   Phase B (upper 16):  for each Phase-A survivor, lift to a 64-bit seed
 *                        by trying every value of the upper 16 bits and
 *                        checking biome and spawn-point constraints.
 *
 * Two scan modes are exposed:
 *
 *   1) Common-seed mode    (32-bit signed range, fast)
 *      For seeds typed by hand (numeric or string-hashed). Treats each
 *      candidate value as a fully-determined int64 (sign-extended) and
 *      runs both phases on it. Default range: [-2^31, 2^31 - 1].
 *
 *   2) Random-seed mode    (48-bit unsigned range, slow)
 *      For seeds Minecraft picked itself with new Random().nextLong().
 *      Iterates the lower 48 bits, then lifts to 64 with biome/spawn
 *      evidence. Without any Phase-B evidence the lower-48 candidates
 *      are reported with a note explaining that the upper 16 bits cannot
 *      be determined.
 *
 * Workers are pthreaded; the build ships a fallback <pthread.h> that runs
 * threads inline on systems that lack a real pthread header (iOS a-Shell),
 * so the code is identical across platforms.
 *
 * ── Checkpointing ─────────────────────────────────────────────────────────
 * Long random-seed runs can be interrupted (Ctrl+C, terminal closed,
 * battery dying) and resumed. Every few seconds the cracker saves its
 * progress and matches found so far to "seedcrackerz.checkpoint" in the
 * working directory. On startup the cracker offers to resume any saved
 * checkpoint. Completion deletes the checkpoint automatically.
 * ────────────────────────────────────────────────────────────────────────
 */

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "finders.h"
#include "generator.h"
#include "biomes.h"

/* ── Constants ──────────────────────────────────────────────────────── */
#define MAX_EV           128
#define MAX_OUT          4096
#define MAX_THREADS      16
#define CHUNK_SIZE       (1ULL << 20)   /* 1M seeds per dispatched work unit */
#define CHECKPOINT_FILE  "seedcrackerz.checkpoint"
#define CHECKPOINT_EVERY 5.0            /* save every N seconds */
#define SPAWN_TOL_DEF    16             /* default spawn-point tolerance, blocks */
/* MASK48 is provided by cubiomes/finders.h. */

/* ── Evidence types ─────────────────────────────────────────────────── */
typedef enum {
    EV_SLIME = 0,    /* Phase A */
    EV_STRUCT = 1,   /* Phase A (incl. buried treasure via Treasure stype) */
    EV_BIOME = 2,    /* Phase B */
    EV_SPAWN = 3     /* Phase B */
} EvType;

typedef struct {
    EvType type;
    union {
        struct { int cx, cz; bool yes; }    sl;
        struct { int stype; int cx, cz; }   st;
        struct { int biome; int bx, bz; }   bm;
        struct { int bx, bz, tol; }         sp;
    };
} Ev;

typedef enum { MODE_COMMON = 0, MODE_RANDOM = 1 } SearchMode;

/* ── Lower-48 evidence checks (Phase A) ─────────────────────────────── */

/* Structure check.
 *
 * cubiomes' getStructurePos returns the structure position in BLOCK
 * coordinates. The previous version of this function compared those
 * blocks to chunk coordinates, which only ever evaluated true at
 * (cx, cz) == (0, 0); structure evidence was effectively a no-op.
 * The fix is to right-shift by 4 to get back to chunks before compare.
 *
 * This routine handles both region-based structures (Village, Pyramid,
 * etc., region size > 1) and single-chunk structures (Treasure, where
 * region size = 1 and getStructurePos returns "no" if this chunk has
 * no buried treasure under the lower-48 RNG).
 */
static bool check_struct(int stype, int cx, int cz, uint64_t seed, int mc)
{
    StructureConfig sc;
    if (!getStructureConfig(stype, mc, &sc))
        return true;   /* structure not valid in this MC version — skip */

    /* Floor division of chunk coord by region size (handles negatives). */
    int regX = (cx < 0 && cx % sc.regionSize) ? cx/sc.regionSize - 1 : cx/sc.regionSize;
    int regZ = (cz < 0 && cz % sc.regionSize) ? cz/sc.regionSize - 1 : cz/sc.regionSize;

    Pos pos;
    if (!getStructurePos(stype, mc, seed, regX, regZ, &pos))
        return false;

    /* pos is in block coords; convert to chunks. Negative block coords
     * fall on a chunk-aligned multiple of 16 by construction here, so a
     * plain arithmetic shift is correct. */
    return (pos.x >> 4) == cx && (pos.z >> 4) == cz;
}

/* Slime chunk check — delegates to cubiomes for the exact Java behaviour
 * (proper rejection-sampling nextInt(10)). */
static bool check_slime(uint64_t seed, int cx, int cz, bool expect_yes)
{
    return (bool)isSlimeChunk(seed, cx, cz) == expect_yes;
}

/* All Phase-A constraints for the lower 48 bits of `seed`. */
static bool phase_a(const Ev *ev, int n_phaseA, uint64_t seed, int mc)
{
    for (int i = 0; i < n_phaseA; i++) {
        const Ev *e = &ev[i];
        if (e->type == EV_STRUCT) {
            if (!check_struct(e->st.stype, e->st.cx, e->st.cz, seed, mc))
                return false;
        } else if (e->type == EV_SLIME) {
            if (!check_slime(seed, e->sl.cx, e->sl.cz, e->sl.yes))
                return false;
        }
    }
    return true;
}

/* All Phase-B constraints (biomes + spawn) for the full 64-bit `seed`. */
static bool phase_b(Generator *gen, const Ev *ev, int nev, int n_phaseA,
                    uint64_t seed, int mc)
{
    bool applied = false;
    for (int i = n_phaseA; i < nev; i++) {
        const Ev *e = &ev[i];
        if (e->type == EV_BIOME) {
            if (!applied) { applySeed(gen, DIM_OVERWORLD, seed); applied = true; }
            int bm;
            if (mc >= MC_1_18)
                bm = getBiomeAt(gen, 4, e->bm.bx >> 2, 16, e->bm.bz >> 2);
            else
                bm = getBiomeAt(gen, 1, e->bm.bx, 0, e->bm.bz);
            if (bm != e->bm.biome) return false;
        } else if (e->type == EV_SPAWN) {
            if (!applied) { applySeed(gen, DIM_OVERWORLD, seed); applied = true; }
            Pos sp = getSpawn(gen);
            int dx = sp.x - e->sp.bx, dz = sp.z - e->sp.bz;
            if (dx*dx + dz*dz > e->sp.tol * e->sp.tol) return false;
        }
    }
    return true;
}

/* ── Input helpers ──────────────────────────────────────────────────── */
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]=0;
}
static int64_t prompt_i64(const char *msg, int64_t def) {
    char buf[64];
    printf("%s [%lld]: ", msg, (long long)def); fflush(stdout);
    if (!fgets(buf,sizeof buf,stdin)) return def;
    chomp(buf); return buf[0] ? strtoll(buf,NULL,10) : def;
}
static uint64_t prompt_u64(const char *msg, uint64_t def) {
    char buf[64];
    printf("%s [%llu]: ", msg, (unsigned long long)def); fflush(stdout);
    if (!fgets(buf,sizeof buf,stdin)) return def;
    chomp(buf); return buf[0] ? strtoull(buf,NULL,10) : def;
}
static int  prompt_int(const char *m, int d)  { return (int)prompt_i64(m,d); }
static char prompt_char(const char *m, char d) {
    char buf[16]; printf("%s [%c]: ",m,d); fflush(stdout);
    if (!fgets(buf,sizeof buf,stdin)) return d;
    chomp(buf); return buf[0] ? buf[0] : d;
}
static void prompt_str(const char *m, const char *d, char *o, size_t cap) {
    printf("%s [%s]: ",m,d); fflush(stdout);
    if (!fgets(o,(int)cap,stdin)) { strncpy(o,d,cap); o[cap-1]=0; return; }
    chomp(o); if (!o[0]) { strncpy(o,d,cap); o[cap-1]=0; }
}

/* ── Structure table ────────────────────────────────────────────────── */
typedef struct { int id; const char *name, *howto; } StInfo;
static const StInfo ST[] = {
    {Village,        "Village",           "Stand at the centre well / main road junction"   },
    {Desert_Pyramid, "Desert_Pyramid",    "Stand in the pressure-plate trap room"           },
    {Jungle_Temple,  "Jungle_Temple",     "Stand in the ground-floor centre room"           },
    {Swamp_Hut,      "Swamp_Hut",         "Stand on the hut floor"                          },
    {Igloo,          "Igloo",             "Stand in the centre of the igloo floor"          },
    {Outpost,        "Pillager_Outpost",  "Stand at the base of the outpost tower"          },
    {Monument,       "Monument",          "Stand in the Elder Guardian central chamber"     },
    {Mansion,        "Woodland_Mansion",  "Stand in the mansion entrance doorway"           },
    {Fortress,       "Nether_Fortress",   "Stand in the first corridor near the entrance"   },
    {Bastion,        "Bastion_Remnant",   "Stand in the centre of the courtyard"            },
    {Shipwreck,      "Shipwreck",         "Stand on the highest deck"                       },
    {Treasure,       "Buried_Treasure",   "Stand on the loot chest you dug up"              },
    {-1,NULL,NULL}
};
static const char *st_name(int id) {
    for (int i=0;ST[i].name;i++) if (ST[i].id==id) return ST[i].name;
    return "?";
}

/* ── Biome table ────────────────────────────────────────────────────── */
typedef struct { int id; const char *name; } BiInfo;
static const BiInfo BI[] = {
    {ocean,"ocean"},{plains,"plains"},{desert,"desert"},{mountains,"mountains"},
    {forest,"forest"},{taiga,"taiga"},{swamp,"swamp"},{frozen_ocean,"frozen_ocean"},
    {snowy_tundra,"snowy_tundra"},{snowy_mountains,"snowy_mountains"},
    {mushroom_fields,"mushroom_fields"},{beach,"beach"},{jungle,"jungle"},
    {jungle_edge,"jungle_edge"},{deep_ocean,"deep_ocean"},{stone_shore,"stone_shore"},
    {birch_forest,"birch_forest"},{dark_forest,"dark_forest"},
    {snowy_taiga,"snowy_taiga"},{savanna,"savanna"},
    {savanna_plateau,"savanna_plateau"},{badlands,"badlands"},
    {warm_ocean,"warm_ocean"},{lukewarm_ocean,"lukewarm_ocean"},
    {cold_ocean,"cold_ocean"},{snowy_beach,"snowy_beach"},
    {giant_tree_taiga,"giant_tree_taiga"},{wooded_mountains,"wooded_mountains"},
    {nether_wastes,"nether_wastes"},{soul_sand_valley,"soul_sand_valley"},
    {crimson_forest,"crimson_forest"},{warped_forest,"warped_forest"},
    {basalt_deltas,"basalt_deltas"},{-1,NULL}
};
static int biome_id(const char *s) {
    for (int i=0;BI[i].name;i++) if (!strcasecmp(BI[i].name,s)) return BI[i].id;
    return -1;
}
static const char *biome_name(int id) {
    for (int i=0;BI[i].name;i++) if (BI[i].id==id) return BI[i].name;
    return "?";
}

/* ── Timing ─────────────────────────────────────────────────────────── */
static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

/* ── Block → chunk (floor division for negative coords) ─────────────── */
static int blk2chk(int b) { return (b<0 && b%16) ? b/16-1 : b/16; }

/* ── Print evidence list ─────────────────────────────────────────────── */
static void print_ev(const Ev *ev, int n) {
    if (!n) { printf("  (none)\n"); return; }
    for (int i=0;i<n;i++) {
        const Ev *e=&ev[i];
        if (e->type==EV_SLIME)
            printf("  [%d] Slime chunk    (%d,%d)  -> %s\n",i+1,e->sl.cx,e->sl.cz,
                   e->sl.yes?"IS slime":"NOT slime");
        else if (e->type==EV_STRUCT)
            printf("  [%d] Structure      %-22s chunk(%d,%d)\n",i+1,
                   st_name(e->st.stype),e->st.cx,e->st.cz);
        else if (e->type==EV_BIOME)
            printf("  [%d] Biome          %-22s block(%d,%d)\n",i+1,
                   biome_name(e->bm.biome),e->bm.bx,e->bm.bz);
        else /* EV_SPAWN */
            printf("  [%d] World spawn    block(%d,%d)  +-%d blocks\n",i+1,
                   e->sp.bx,e->sp.bz,e->sp.tol);
    }
}

/* ── Sort evidence: Phase A first, Phase B last ─────────────────────── */
static int phase_of(EvType t) {
    /* Within Phase A: slime (cheapest) before struct.
     * Within Phase B: biome (cheapest) before spawn (expensive). */
    switch (t) {
        case EV_SLIME:  return 0;
        case EV_STRUCT: return 1;
        case EV_BIOME:  return 2;
        case EV_SPAWN:  return 3;
    }
    return 9;
}
static void sort_ev(Ev *ev, int n) {
    for (int i=0;i<n;i++) for (int j=i+1;j<n;j++)
        if (phase_of(ev[j].type) < phase_of(ev[i].type)) {
            Ev t=ev[i]; ev[i]=ev[j]; ev[j]=t;
        }
}

/* ── Shared state ───────────────────────────────────────────────────── */
typedef struct {
    pthread_mutex_t mu;
    uint64_t  cursor;        /* index of next seed to dispatch (0..total) */
    uint64_t  completed;     /* seeds finished — for progress display */
    uint64_t  total;
} WorkQueue;

typedef struct {
    pthread_mutex_t mu;
    uint64_t out[MAX_OUT];
    int      nout;
} OutBuf;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ── Worker ─────────────────────────────────────────────────────────── */
typedef struct {
    SearchMode mode;
    int        mc;

    int64_t   s0_signed;     /* MODE_COMMON: lowest seed value in range */
    uint64_t  s0_u48;        /* MODE_RANDOM: lowest lower-48 in range */

    const Ev *ev;
    int       nev;
    int       n_phaseA;      /* slime + structure (incl. treasure) */
    int       n_phaseB;      /* biome + spawn */

    WorkQueue *wq;
    OutBuf    *ob;
} Worker;

static void emit(OutBuf *ob, uint64_t seed)
{
    pthread_mutex_lock(&ob->mu);
    if (ob->nout < MAX_OUT) ob->out[ob->nout++] = seed;
    pthread_mutex_unlock(&ob->mu);
}

static void *worker_main(void *arg)
{
    Worker *w = (Worker *)arg;

    /* Each thread needs its own generator (cubiomes generators are not
     * thread-safe — they own mutable biome scratch buffers). */
    Generator gen;
    setupGenerator(&gen, w->mc, 0);

    for (;;) {
        if (g_stop) break;

        pthread_mutex_lock(&w->wq->mu);
        uint64_t my_start = w->wq->cursor;
        if (my_start >= w->wq->total) {
            pthread_mutex_unlock(&w->wq->mu);
            break;
        }
        uint64_t my_count = w->wq->total - my_start;
        if (my_count > CHUNK_SIZE) my_count = CHUNK_SIZE;
        w->wq->cursor += my_count;
        pthread_mutex_unlock(&w->wq->mu);

        /* Process this chunk of indices. */
        for (uint64_t i = 0; i < my_count; i++) {
            uint64_t idx = my_start + i;

            if (w->mode == MODE_COMMON) {
                uint64_t seed = (uint64_t)((int64_t)w->s0_signed + (int64_t)idx);
                if (phase_a(w->ev, w->n_phaseA, seed, w->mc)) {
                    if (w->n_phaseB == 0 ||
                        phase_b(&gen, w->ev, w->nev, w->n_phaseA, seed, w->mc))
                        emit(w->ob, seed);
                }
            } else { /* MODE_RANDOM */
                uint64_t s48 = w->s0_u48 + idx;
                if (phase_a(w->ev, w->n_phaseA, s48, w->mc)) {
                    if (w->n_phaseB > 0) {
                        for (uint32_t up = 0; up < 65536u; up++) {
                            if (g_stop) break;
                            uint64_t seed = ((uint64_t)up << 48) | s48;
                            if (phase_b(&gen, w->ev, w->nev, w->n_phaseA, seed, w->mc))
                                emit(w->ob, seed);
                        }
                    } else {
                        emit(w->ob, s48);
                    }
                }
            }

            /* Honour Ctrl+C mid-chunk so users don't wait through a
             * full 1M-seed chunk before the cracker exits. */
            if ((i & 0xFFFFu) == 0xFFFFu && g_stop) {
                /* Bail out early. The portion we've done counts toward
                 * `completed`, but we haven't tracked it yet — rather
                 * than complicate accounting, just stop and re-do this
                 * chunk on resume. Duplicates are removed at print. */
                return NULL;
            }
        }

        pthread_mutex_lock(&w->wq->mu);
        w->wq->completed += my_count;
        pthread_mutex_unlock(&w->wq->mu);
    }
    return NULL;
}

/* ── Checkpoint I/O ─────────────────────────────────────────────────── */

typedef struct {
    int        version;       /* must be 1 */
    int        mc;
    SearchMode mode;
    int64_t    s0_signed, s1_signed;   /* MODE_COMMON */
    uint64_t   s0_u48,    s1_u48;      /* MODE_RANDOM */
    int        nev;
    Ev         ev[MAX_EV];
    uint64_t   completed;     /* seeds completed so far */
    int        nout;
    uint64_t   out[MAX_OUT];
} Checkpoint;

static void chkpt_save(const Checkpoint *c)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", CHECKPOINT_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "SCZ_CKP_V1\n");
    fprintf(f, "mc=%d\n", c->mc);
    fprintf(f, "mode=%d\n", (int)c->mode);
    if (c->mode == MODE_COMMON)
        fprintf(f, "range=%lld:%lld\n", (long long)c->s0_signed, (long long)c->s1_signed);
    else
        fprintf(f, "range=%llu:%llu\n",
                (unsigned long long)c->s0_u48, (unsigned long long)c->s1_u48);
    fprintf(f, "nev=%d\n", c->nev);
    for (int i = 0; i < c->nev; i++) {
        const Ev *e = &c->ev[i];
        switch (e->type) {
            case EV_SLIME:
                fprintf(f, "ev=slime:%d,%d,%d\n", e->sl.cx, e->sl.cz, e->sl.yes?1:0);
                break;
            case EV_STRUCT:
                fprintf(f, "ev=struct:%d,%d,%d\n", e->st.stype, e->st.cx, e->st.cz);
                break;
            case EV_BIOME:
                fprintf(f, "ev=biome:%d,%d,%d\n", e->bm.biome, e->bm.bx, e->bm.bz);
                break;
            case EV_SPAWN:
                fprintf(f, "ev=spawn:%d,%d,%d\n", e->sp.bx, e->sp.bz, e->sp.tol);
                break;
        }
    }
    fprintf(f, "completed=%llu\n", (unsigned long long)c->completed);
    fprintf(f, "nout=%d\n", c->nout);
    for (int i = 0; i < c->nout; i++)
        fprintf(f, "out=%llu\n", (unsigned long long)c->out[i]);
    fclose(f);
    rename(tmp, CHECKPOINT_FILE);
}

/* Try to load a checkpoint. Returns 1 on success, 0 if missing/invalid.
 *
 * We do this in two passes. The first pass picks up the scalar header
 * fields (version, mc, mode, range, nev, completed). The second pass
 * appends the variable-length lists (ev=..., out=...) using local
 * counters, since their order in the file matters and we want the
 * final c->nev / c->nout to reflect what was actually parsed (in case
 * the file was truncated at write time).
 */
static int chkpt_load(Checkpoint *c)
{
    FILE *f = fopen(CHECKPOINT_FILE, "r");
    if (!f) return 0;

    memset(c, 0, sizeof *c);
    c->version = 1;

    char line[256];
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    chomp(line);
    if (strcmp(line, "SCZ_CKP_V1") != 0) { fclose(f); return 0; }

    /* Pass 1: scalar header fields. */
    int mode_int = 0;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (sscanf(line, "mc=%d", &c->mc) == 1) continue;
        if (sscanf(line, "mode=%d", &mode_int) == 1) { c->mode = (SearchMode)mode_int; continue; }
        if (strncmp(line, "range=", 6) == 0) {
            if (c->mode == MODE_COMMON) {
                long long a, b;
                if (sscanf(line+6, "%lld:%lld", &a, &b) == 2) {
                    c->s0_signed = a; c->s1_signed = b;
                }
            } else {
                unsigned long long a, b;
                if (sscanf(line+6, "%llu:%llu", &a, &b) == 2) {
                    c->s0_u48 = a; c->s1_u48 = b;
                }
            }
            continue;
        }
        if (sscanf(line, "completed=%llu", (unsigned long long *)&c->completed) == 1) continue;
        /* nev / nout / ev / out are handled in pass 2. */
    }

    /* Pass 2: variable-length lists, appended in file order. */
    rewind(f);
    int evi = 0, outi = 0;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (strncmp(line, "ev=slime:", 9) == 0) {
            int cx, cz, yes;
            if (sscanf(line+9, "%d,%d,%d", &cx, &cz, &yes) == 3 && evi < MAX_EV) {
                c->ev[evi].type = EV_SLIME;
                c->ev[evi].sl.cx = cx; c->ev[evi].sl.cz = cz;
                c->ev[evi].sl.yes = yes ? true : false;
                evi++;
            }
        } else if (strncmp(line, "ev=struct:", 10) == 0) {
            int st, cx, cz;
            if (sscanf(line+10, "%d,%d,%d", &st, &cx, &cz) == 3 && evi < MAX_EV) {
                c->ev[evi].type = EV_STRUCT;
                c->ev[evi].st.stype = st; c->ev[evi].st.cx = cx; c->ev[evi].st.cz = cz;
                evi++;
            }
        } else if (strncmp(line, "ev=biome:", 9) == 0) {
            int bid, bx, bz;
            if (sscanf(line+9, "%d,%d,%d", &bid, &bx, &bz) == 3 && evi < MAX_EV) {
                c->ev[evi].type = EV_BIOME;
                c->ev[evi].bm.biome = bid; c->ev[evi].bm.bx = bx; c->ev[evi].bm.bz = bz;
                evi++;
            }
        } else if (strncmp(line, "ev=spawn:", 9) == 0) {
            int bx, bz, tol;
            if (sscanf(line+9, "%d,%d,%d", &bx, &bz, &tol) == 3 && evi < MAX_EV) {
                c->ev[evi].type = EV_SPAWN;
                c->ev[evi].sp.bx = bx; c->ev[evi].sp.bz = bz; c->ev[evi].sp.tol = tol;
                evi++;
            }
        } else if (strncmp(line, "out=", 4) == 0) {
            unsigned long long v;
            if (sscanf(line+4, "%llu", &v) == 1 && outi < MAX_OUT) {
                c->out[outi++] = v;
            }
        }
    }
    c->nev  = evi;
    c->nout = outi;

    fclose(f);
    return 1;
}

static void chkpt_delete(void) { unlink(CHECKPOINT_FILE); }

/* Print a one-line summary of a checkpoint for the user. */
static void chkpt_summarise(const Checkpoint *c, uint64_t total)
{
    const char *modename = c->mode == MODE_COMMON ? "Common-seed (32-bit signed)"
                                                   : "Random-seed (48-bit)";
    printf("  Mode:         %s\n", modename);
    printf("  Minecraft:    1.%d\n",
           c->mc == MC_1_21 ? 21 : c->mc == MC_1_20 ? 20 :
           c->mc == MC_1_19 ? 19 : c->mc == MC_1_18 ? 18 :
           c->mc == MC_1_17 ? 17 : c->mc == MC_1_16 ? 16 : 0);
    if (c->mode == MODE_COMMON)
        printf("  Range:        %lld .. %lld\n",
               (long long)c->s0_signed, (long long)c->s1_signed);
    else
        printf("  Range:        %llu .. %llu (lower-48)\n",
               (unsigned long long)c->s0_u48, (unsigned long long)c->s1_u48);
    printf("  Evidence:     %d item(s)\n", c->nev);
    if (total)
        printf("  Progress:     %llu / %llu  (%.2f%%)\n",
               (unsigned long long)c->completed, (unsigned long long)total,
               100.0 * (double)c->completed / (double)total);
    else
        printf("  Progress:     %llu seeds completed\n",
               (unsigned long long)c->completed);
    printf("  Matches saved: %d\n", c->nout);
}

/* ── Progress reporter ──────────────────────────────────────────────── */
static void print_bar(double frac, double el, int found, double speed_Mps)
{
    if (frac > 1.0) frac = 1.0;
    int pct  = (int)(frac * 100 + 0.5);
    int bars = (int)(frac * 30 + 0.5);
    char bar[32];
    for (int b = 0; b < 30; b++) bar[b] = b < bars ? '#' : '-';
    bar[30] = 0;
    double eta = frac > 0.001 ? el * (1.0/frac - 1.0) : 0.0;
    printf("\r  [%s] %3d%%  %7.1fs  eta %7.1fs  %5.1f M/s  found %-5d",
           bar, pct, el, eta, speed_Mps, found);
    fflush(stdout);
}

/* ── Compare for qsort (uint64) ─────────────────────────────────────── */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ── Main ───────────────────────────────────────────────────────────── */
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== SeedCrackerZ ===  Java Edition seed cracker (POSIX terminal)\n");
    printf("    by Batthepig  |  powered by cubiomes\n\n");

    /* Install Ctrl+C handler so we can save a checkpoint on exit. */
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* ── 0. Resume? ────────────────────────────────────────────────── */
    Checkpoint resume_ckpt;
    bool resume = false;
    if (chkpt_load(&resume_ckpt)) {
        uint64_t resume_total =
            (resume_ckpt.mode == MODE_COMMON)
                ? (uint64_t)(resume_ckpt.s1_signed - resume_ckpt.s0_signed) + 1ULL
                : (resume_ckpt.s1_u48 - resume_ckpt.s0_u48) + 1ULL;
        printf("Found a saved checkpoint:\n");
        chkpt_summarise(&resume_ckpt, resume_total);
        char yn = prompt_char("Resume this run? (y/n)", 'y');
        printf("\n");
        if (yn == 'y' || yn == 'Y') {
            resume = true;
        } else {
            chkpt_delete();
        }
    }

    /* These are the working values for this run, populated either from
     * resume_ckpt or from interactive prompts below. */
    int      mc = MC_1_21;
    const char *mcname = "1.21";
    static Ev ev[MAX_EV];
    int       nev = 0;
    SearchMode mode = MODE_COMMON;
    int64_t  s0_signed = 0, s1_signed = 0;
    uint64_t s0_u48 = 0,    s1_u48 = 0;
    uint64_t resume_completed = 0;
    int      preloaded_nout = 0;
    static uint64_t preloaded_out[MAX_OUT];

    if (resume) {
        mc   = resume_ckpt.mc;
        mode = resume_ckpt.mode;
        nev  = resume_ckpt.nev;
        for (int i = 0; i < nev; i++) ev[i] = resume_ckpt.ev[i];
        s0_signed = resume_ckpt.s0_signed; s1_signed = resume_ckpt.s1_signed;
        s0_u48    = resume_ckpt.s0_u48;    s1_u48    = resume_ckpt.s1_u48;
        resume_completed = resume_ckpt.completed;
        preloaded_nout = resume_ckpt.nout;
        for (int i = 0; i < preloaded_nout; i++) preloaded_out[i] = resume_ckpt.out[i];
        switch (mc) {
            case MC_1_21: mcname = "1.21"; break;
            case MC_1_20: mcname = "1.20"; break;
            case MC_1_19: mcname = "1.19"; break;
            case MC_1_18: mcname = "1.18"; break;
            case MC_1_17: mcname = "1.17"; break;
            case MC_1_16: mcname = "1.16"; break;
        }
    } else {
        /* 1. Minecraft version */
        printf("Minecraft version:\n");
        printf("  1) 1.21   2) 1.20   3) 1.19   4) 1.18   5) 1.17   6) 1.16\n");
        int mvc=prompt_int("Choice",1);
        const int   MCMAP[] = {MC_1_21,MC_1_20,MC_1_19,MC_1_18,MC_1_17,MC_1_16};
        const char *MCNM[]  = {"1.21","1.20","1.19","1.18","1.17","1.16"};
        if (mvc<1||mvc>6) mvc=1;
        mc=MCMAP[mvc-1];
        mcname=MCNM[mvc-1];

        /* 2. Collect evidence */
        printf("\n-- Evidence Collection -----------------------------------------------\n");
        printf("Commands:  s=structure  c=slime chunk  b=biome  w=world spawn\n");
        printf("           p=print list  d=done\n");
        printf("Note:      buried treasure is option 12 under the structure menu (s).\n\n");

        for (;;) {
            printf("Add evidence: "); fflush(stdout);
            char line[8];
            if (!fgets(line,sizeof line,stdin)) break;
            chomp(line);
            char cmd=line[0];

            if (cmd=='d') {
                if (!nev) { printf("Need at least one piece of evidence.\n\n"); continue; }
                break;
            }
            if (cmd=='p') {
                printf("\nEvidence (%d):\n",nev);
                print_ev(ev,nev);
                printf("\n");
                continue;
            }

            if (cmd=='s' && nev<MAX_EV) {
                printf("\nStructure types:\n");
                for (int i=0;ST[i].name;i++)
                    printf("  %2d) %-22s  (%s)\n",i+1,ST[i].name,ST[i].howto);
                int sc=prompt_int("Type number",1);
                if (sc<1||!ST[sc-1].name) { printf("Invalid.\n\n"); continue; }
                printf("Block coordinates of your position inside the structure (from F3):\n");
                printf("  -> %s\n",ST[sc-1].howto);
                int bx=prompt_int("Block X",0);
                int bz=prompt_int("Block Z",0);
                int cx=blk2chk(bx), cz=blk2chk(bz);
                ev[nev++]=(Ev){.type=EV_STRUCT,.st={ST[sc-1].id,cx,cz}};
                printf("  Stored as chunk(%d,%d). Added.\n\n",cx,cz);
                continue;
            }

            if (cmd=='c' && nev<MAX_EV) {
                printf("\nDig a room below Y=40 (any biome). Wait two Minecraft nights.\n");
                printf("F3+G shows chunk boundaries. F3 'Chunk' absolute = chunk X/Z.\n");
                int cx=prompt_int("Chunk X",0);
                int cz=prompt_int("Chunk Z",0);
                char yn=prompt_char("Slimes spawn here? (y/n)",'y');
                bool yes=(yn=='y'||yn=='Y');
                ev[nev++]=(Ev){.type=EV_SLIME,.sl={cx,cz,yes}};
                printf("  Chunk(%d,%d) %s a slime chunk. Added.\n\n",
                       cx,cz,yes?"IS":"is NOT");
                continue;
            }

            if (cmd=='b' && nev<MAX_EV) {
                printf("\nBiomes: plains desert forest taiga jungle swamp ocean savanna\n");
                printf("        badlands dark_forest snowy_tundra nether_wastes\n");
                printf("        crimson_forest warped_forest soul_sand_valley basalt_deltas\n");
                char bname[64];
                prompt_str("Biome name","plains",bname,sizeof bname);
                int bid=biome_id(bname);
                if (bid<0) { printf("Unknown biome '%s'. Try again.\n\n",bname); continue; }
                int bx=prompt_int("Block X (anywhere in that biome)",0);
                int bz=prompt_int("Block Z (anywhere in that biome)",0);
                ev[nev++]=(Ev){.type=EV_BIOME,.bm={bid,bx,bz}};
                printf("  %s at block(%d,%d). Added.\n\n",bname,bx,bz);
                continue;
            }

            if (cmd=='w' && nev<MAX_EV) {
                printf("\nWorld spawn point. To find it:\n");
                printf("  1) Sleep in any bed and break it -- you respawn at world spawn.\n");
                printf("     (Or: as operator, run /tp @s 0 0 0 then /spawnpoint to read it.)\n");
                printf("  2) Read your block X and Z from F3.\n");
                printf("Tolerance: world-spawn search picks a random nearby block, so a\n");
                printf("few blocks of slack help. Default is %d blocks.\n", SPAWN_TOL_DEF);
                int bx=prompt_int("Spawn block X",0);
                int bz=prompt_int("Spawn block Z",0);
                int tol=prompt_int("Tolerance (blocks)",SPAWN_TOL_DEF);
                if (tol < 0) tol = 0;
                ev[nev++]=(Ev){.type=EV_SPAWN,.sp={bx,bz,tol}};
                printf("  Spawn at block(%d,%d) +-%d blocks. Added.\n\n",bx,bz,tol);
                continue;
            }

            if (nev>=MAX_EV) printf("Evidence list full (%d).\n\n",MAX_EV);
            else printf("Unknown command.\n\n");
        }

        /* 3. Search mode */
        printf("\n-- Search Mode -------------------------------------------------------\n");
        printf("  1) Common-seed mode  (signed 32-bit, fast)\n");
        printf("       Hand-typed numeric seeds, hashed string seeds, or any\n");
        printf("       seed that fits in a signed int. Default range:\n");
        printf("       [-2147483648 .. 2147483647]. Typically <1 minute.\n\n");
        printf("  2) Random-seed mode  (unsigned 48-bit, slow)\n");
        printf("       Seeds Minecraft picked itself with new Random().nextLong().\n");
        printf("       Iterates the lower 48 bits, then lifts to 64 bits via\n");
        printf("       biome/spawn evidence. Hours to days; needs 2+ structures\n");
        printf("       or 3+ slime chunks for tractable runtime.\n\n");
        int mc_choice = prompt_int("Choice", 1);
        mode = (mc_choice == 2) ? MODE_RANDOM : MODE_COMMON;

        /* 4. Range */
        if (mode == MODE_COMMON) {
            printf("\nSeed range:\n");
            s0_signed = prompt_i64("Range start", -2147483648LL);
            s1_signed = prompt_i64("Range end",    2147483647LL);
            if (s1_signed < s0_signed) { int64_t t = s0_signed; s0_signed = s1_signed; s1_signed = t; }
        } else {
            printf("\nLower-48 range (unsigned, max %llu):\n",
                   (unsigned long long)MASK48);
            s0_u48 = prompt_u64("Range start", 0ULL);
            s1_u48 = prompt_u64("Range end",   MASK48);
            if (s0_u48 > MASK48) s0_u48 = MASK48;
            if (s1_u48 > MASK48) s1_u48 = MASK48;
            if (s1_u48 < s0_u48) { uint64_t t = s0_u48; s0_u48 = s1_u48; s1_u48 = t; }
        }
    }

    /* Sort + count phase A/B. */
    sort_ev(ev, nev);
    int n_struct = 0, n_slime = 0, n_biome = 0, n_spawn = 0;
    for (int i=0;i<nev;i++) {
        switch (ev[i].type) {
            case EV_STRUCT: n_struct++; break;
            case EV_SLIME:  n_slime++;  break;
            case EV_BIOME:  n_biome++;  break;
            case EV_SPAWN:  n_spawn++;  break;
        }
    }
    int n_phaseA = n_struct + n_slime;
    int n_phaseB = n_biome + n_spawn;

    /* Total seeds in the dispatched range. */
    uint64_t total =
        (mode == MODE_COMMON)
            ? (uint64_t)(s1_signed - s0_signed) + 1ULL
            : (s1_u48 - s0_u48) + 1ULL;

    /* 5. Thread count */
    int max_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (max_cores < 1) max_cores = 1;
    if (max_cores > MAX_THREADS) max_cores = MAX_THREADS;
    int n_threads = prompt_int("Threads (1 = single-threaded)", max_cores);
    if (n_threads < 1) n_threads = 1;
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;
    if ((uint64_t)n_threads > total) n_threads = (int)total;

    printf("\n%d evidence item(s) | %.3f B seeds | Java %s | %d thread(s)%s\n",
           nev, total/1e9, mcname, n_threads,
           resume ? "  [RESUMING]" : "");
    printf("Phase A (lower-48): %d structure + %d slime\n", n_struct, n_slime);
    printf("Phase B (upper-16): %d biome + %d spawn\n", n_biome, n_spawn);
    print_ev(ev,nev);
    printf("\n");

    if (mode == MODE_RANDOM && n_phaseA < 2 && total > (1ULL << 36)) {
        printf("WARNING: random-seed mode with fewer than 2 Phase-A constraints over\n");
        printf("         a wide range will take days to weeks. Add evidence or narrow\n");
        printf("         the range with Range start / Range end.\n\n");
    }
    if (mode == MODE_RANDOM && n_phaseA > 0 && n_phaseB == 0) {
        printf("Note: no biome/spawn evidence -- the cracker will report the lower-48\n");
        printf("      candidate values (each represents 65536 possible 64-bit seeds).\n\n");
    }

    /* 6. Set up shared state. */
    OutBuf ob = { .nout = 0 };
    pthread_mutex_init(&ob.mu, NULL);
    /* Pre-seed the output buffer with matches from the resumed checkpoint,
     * if any. We dedupe at the end so re-emitted matches are harmless. */
    for (int i = 0; i < preloaded_nout && ob.nout < MAX_OUT; i++)
        ob.out[ob.nout++] = preloaded_out[i];

    WorkQueue wq = { .cursor = resume_completed, .completed = resume_completed,
                     .total = total };
    pthread_mutex_init(&wq.mu, NULL);

    /* 7. Build worker descriptors and launch. */
    Worker  workers[MAX_THREADS];
    pthread_t tids[MAX_THREADS];
    for (int i = 0; i < n_threads; i++) {
        workers[i] = (Worker){
            .mode = mode, .mc = mc,
            .s0_signed = s0_signed, .s0_u48 = s0_u48,
            .ev = ev, .nev = nev,
            .n_phaseA = n_phaseA, .n_phaseB = n_phaseB,
            .wq = &wq, .ob = &ob,
        };
    }

    double t0 = now_sec();
    double last_chkpt = t0;
    for (int i = 0; i < n_threads; i++)
        pthread_create(&tids[i], NULL, worker_main, &workers[i]);

    /* 8. Live progress loop with periodic checkpointing. */
    for (;;) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&wq.mu);
        uint64_t done = wq.completed;
        pthread_mutex_unlock(&wq.mu);

        pthread_mutex_lock(&ob.mu);
        int found = ob.nout;
        pthread_mutex_unlock(&ob.mu);

        double el = now_sec() - t0;
        double frac = total ? (double)done / (double)total : 1.0;
        double speed_Mps = el > 0.0 ? (double)(done - resume_completed) / el / 1e6 : 0.0;
        print_bar(frac, el, found, speed_Mps);

        /* Save a checkpoint every CHECKPOINT_EVERY seconds. */
        double tnow = now_sec();
        if (tnow - last_chkpt >= CHECKPOINT_EVERY && done < total && !g_stop) {
            Checkpoint c = {
                .version = 1, .mc = mc, .mode = mode,
                .s0_signed = s0_signed, .s1_signed = s1_signed,
                .s0_u48 = s0_u48,       .s1_u48    = s1_u48,
                .nev = nev,
                .completed = done,
                .nout = found,
            };
            for (int i = 0; i < nev; i++) c.ev[i] = ev[i];
            pthread_mutex_lock(&ob.mu);
            for (int i = 0; i < found && i < MAX_OUT; i++) c.out[i] = ob.out[i];
            pthread_mutex_unlock(&ob.mu);
            chkpt_save(&c);
            last_chkpt = tnow;
        }

        if (done >= total) break;
        if (g_stop) break;
    }

    for (int i = 0; i < n_threads; i++)
        pthread_join(tids[i], NULL);

    double dt = now_sec() - t0;
    pthread_mutex_lock(&wq.mu);
    uint64_t final_done = wq.completed;
    pthread_mutex_unlock(&wq.mu);

    /* Final progress line. */
    print_bar(total ? (double)final_done / (double)total : 1.0,
              dt, ob.nout,
              dt > 0.0 ? (double)(final_done - resume_completed) / dt / 1e6 : 0.0);
    printf("\n\n");

    /* Save / clear checkpoint depending on completion. */
    if (g_stop && final_done < total) {
        Checkpoint c = {
            .version = 1, .mc = mc, .mode = mode,
            .s0_signed = s0_signed, .s1_signed = s1_signed,
            .s0_u48 = s0_u48,       .s1_u48    = s1_u48,
            .nev = nev,
            .completed = final_done,
            .nout = ob.nout,
        };
        for (int i = 0; i < nev; i++) c.ev[i] = ev[i];
        for (int i = 0; i < ob.nout && i < MAX_OUT; i++) c.out[i] = ob.out[i];
        chkpt_save(&c);
        printf("Interrupted. Progress saved to %s.\n", CHECKPOINT_FILE);
        printf("Run ./seedcrackerz again to resume.\n\n");
    } else {
        chkpt_delete();
        printf("Finished in %.2fs.\n\n", dt);
    }

    pthread_mutex_destroy(&wq.mu);
    pthread_mutex_destroy(&ob.mu);

    /* Dedupe matches (resumed runs may re-emit a few during chunk redo). */
    qsort(ob.out, ob.nout, sizeof(uint64_t), cmp_u64);
    int unique = 0;
    for (int i = 0; i < ob.nout; i++) {
        if (i == 0 || ob.out[i] != ob.out[i-1]) ob.out[unique++] = ob.out[i];
    }
    ob.nout = unique;

    /* 9. Results */
    if (!ob.nout) {
        printf("No seeds matched.\n\n");
        printf("Troubleshooting:\n");
        printf("  1. Re-read F3 coordinates -- one wrong digit rules out the real seed.\n");
        printf("  2. Confirm Minecraft version matches when the world was created.\n");
        printf("  3. For structures: try +-16 blocks (one chunk) in each direction.\n");
        printf("     You may not be standing in the origin chunk.\n");
        printf("  4. For slime chunks: was your test below Y=40 in a cave, not swamp?\n");
        printf("     Swamp slimes spawn everywhere and don't follow the chunk rule.\n");
        printf("  5. For world spawn: try increasing the tolerance to 32 or 64 blocks.\n");
        printf("  6. If you used Common-seed mode and your world was created without\n");
        printf("     a custom seed, switch to Random-seed mode (option 2).\n");
    } else if (mode == MODE_RANDOM && n_phaseB == 0) {
        printf("Matching lower-48-bit seed value(s):\n\n");
        for (int i=0;i<ob.nout;i++) printf("  %llu\n", (unsigned long long)ob.out[i]);
        printf("\nNo biome or spawn evidence was provided, so the upper 16 bits of\n");
        printf("the world seed cannot be determined. Each lower-48 value above\n");
        printf("corresponds to 65536 possible 64-bit world seeds. Add a biome sample\n");
        printf("or spawn-point reading and re-run to pin down the exact world seed.\n");
    } else {
        printf("Matching seed(s):\n\n");
        for (int i=0;i<ob.nout;i++) printf("  %lld\n", (long long)ob.out[i]);
        printf("\n");
        if (ob.nout == 1)
            printf("One match -- almost certainly your world seed. Verify with /seed in-game.\n");
        else {
            printf("%d candidates. To narrow down:\n",ob.nout);
            printf("  - Add a world-spawn reading (powerful Phase-B filter).\n");
            printf("  - Add a biome observation.\n");
            printf("  - Add more slime-chunk or structure evidence.\n");
        }
        if (ob.nout >= MAX_OUT)
            printf("\n(output buffer was capped at %d -- there may be more matches)\n",
                   MAX_OUT);
    }

    return 0;
}
