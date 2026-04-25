/*
 * seedcrackerz.c  —  Java Edition world seed cracker for any POSIX terminal.
 *
 * Provide structure locations, biome observations and/or slime-chunk results
 * collected while playing, then let SeedCrackerZ search a seed range to find
 * which world seed(s) produce exactly that world.
 *
 * Powered by the cubiomes library (https://github.com/cubitect/cubiomes)
 * which implements Mojang's exact world-generation algorithms in C.
 *
 * Build:   make             (downloads cubiomes automatically, then compiles)
 * Run:     ./seedcrackerz
 *
 * ── Architecture ──────────────────────────────────────────────────────────
 * Java's RNG is internally 48-bit. Structure placement and slime chunks
 * therefore depend ONLY on the lower 48 bits of the world seed. Biome
 * generation uses the full 64 bits.
 *
 * The search is split into two phases accordingly:
 *
 *   Phase A (lower 48):  iterate candidate lower-48 values, keep only those
 *                        that satisfy every structure and slime constraint.
 *   Phase B (upper 16):  for each Phase-A survivor, lift to a 64-bit seed
 *                        by trying every value of the upper 16 bits and
 *                        checking biome constraints.
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
 *      Iterates the lower 48 bits, then lifts to 64 with biome evidence.
 *      Without biome evidence the lower-48 candidates are reported with
 *      a note explaining that the upper 16 bits cannot be determined.
 *
 * Workers are pthreaded; the build ships a fallback <pthread.h> that runs
 * threads inline on systems that lack a real pthread header (iOS a-Shell),
 * so the code is identical across platforms.
 *
 * ── Evidence quality guide ────────────────────────────────────────────────
 *   Fast (seconds, common-seed mode):
 *     2+ different structure types
 *   Good (seconds to minutes, common-seed mode):
 *     1 structure + 1 biome sample   OR   3+ slime-chunk results
 *   Slow (hours, random-seed mode):
 *     2+ structures (early Phase-A rejection makes runtime tolerable)
 *   Impractical without smarter algorithms:
 *     random-seed mode with only a single structure
 * ────────────────────────────────────────────────────────────────────────
 */

#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
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
#define MAX_EV       64
#define MAX_OUT      4096
#define MAX_THREADS  16
/* MASK48 is provided by cubiomes/finders.h. */

/* ── Evidence types ─────────────────────────────────────────────────── */
typedef enum { EV_SLIME, EV_STRUCT, EV_BIOME } EvType;

typedef struct {
    EvType type;
    union {
        struct { int cx, cz; bool yes; }  sl;
        struct { int stype; int cx, cz; } st;
        struct { int biome; int bx, bz; } bm;
    };
} Ev;

typedef enum { MODE_COMMON, MODE_RANDOM } SearchMode;

/* ── Lower-48 evidence checks (Phase A) ─────────────────────────────── */

/* Structure check.
 *
 * cubiomes' getStructurePos returns the structure position in BLOCK
 * coordinates. The previous version of this function compared those
 * blocks to chunk coordinates, which only ever evaluated true at
 * (cx, cz) == (0, 0); structure evidence was effectively a no-op.
 * The fix is to right-shift by 4 to get back to chunks before compare.
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
static bool phase_a(const Ev *ev, int n_lower48, uint64_t seed, int mc)
{
    for (int i = 0; i < n_lower48; i++) {
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

/* All Phase-B constraints (biomes) for the full 64-bit `seed`. */
static bool phase_b(Generator *gen, const Ev *ev, int nev, int n_lower48,
                    uint64_t seed, int mc)
{
    bool applied = false;
    for (int i = n_lower48; i < nev; i++) {
        const Ev *e = &ev[i];
        if (e->type != EV_BIOME) continue;
        if (!applied) { applySeed(gen, DIM_OVERWORLD, seed); applied = true; }
        int bm;
        if (mc >= MC_1_18)
            bm = getBiomeAt(gen, 4, e->bm.bx >> 2, 16, e->bm.bz >> 2);
        else
            bm = getBiomeAt(gen, 1, e->bm.bx, 0, e->bm.bz);
        if (bm != e->bm.biome) return false;
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
    {Igloo,          "Igloo",             "Stand in the centre of the igloo floor"           },
    {Outpost,        "Pillager_Outpost",  "Stand at the base of the outpost tower"          },
    {Monument,       "Monument",          "Stand in the Elder Guardian central chamber"     },
    {Mansion,        "Woodland_Mansion",  "Stand in the mansion entrance doorway"           },
    {Fortress,       "Nether_Fortress",   "Stand in the first corridor near the entrance"   },
    {Bastion,        "Bastion_Remnant",   "Stand in the centre of the courtyard"            },
    {Shipwreck,      "Shipwreck",         "Stand on the highest deck"                       },
    {-1,NULL,NULL}
};

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
            printf("  [%d] Slime chunk  (%d,%d)  -> %s\n",i+1,e->sl.cx,e->sl.cz,
                   e->sl.yes?"IS slime":"NOT slime");
        else if (e->type==EV_STRUCT) {
            const char *nm="?";
            for (int j=0;ST[j].name;j++) if(ST[j].id==e->st.stype){nm=ST[j].name;break;}
            printf("  [%d] Structure  %-22s chunk(%d,%d)\n",i+1,nm,e->st.cx,e->st.cz);
        } else {
            printf("  [%d] Biome  %-22s block(%d,%d)\n",i+1,
                   biome_name(e->bm.biome),e->bm.bx,e->bm.bz);
        }
    }
}

/* ── Worker thread state ────────────────────────────────────────────── */
typedef struct {
    /* Sub-range to scan, semantics depends on `mode`. */
    int64_t  sub_s0_signed, sub_s1_signed;  /* MODE_COMMON */
    uint64_t sub_s0_u48,    sub_s1_u48;     /* MODE_RANDOM */

    SearchMode mode;
    int        mc;

    const Ev *ev;
    int       nev;
    int       n_lower48;   /* # of structure + slime evidence (sorted first) */
    int       n_biome;     /* # of biome evidence (the rest) */

    /* Shared output buffer */
    pthread_mutex_t *out_mu;
    uint64_t        *out;
    int             *nout;
    int              ocap;

    /* Shared progress counter (in-flight seeds) */
    pthread_mutex_t *prog_mu;
    uint64_t        *progress;
} Worker;

/* Append a match to the shared output buffer (mutex-protected). */
static void emit(Worker *w, uint64_t seed)
{
    pthread_mutex_lock(w->out_mu);
    if (*w->nout < w->ocap) w->out[(*w->nout)++] = seed;
    pthread_mutex_unlock(w->out_mu);
}

/* Worker loop. */
static void *worker_main(void *arg)
{
    Worker *w = (Worker *)arg;

    /* Each thread needs its own generator (cubiomes generators are not
     * thread-safe — they own mutable biome scratch buffers). */
    Generator gen;
    setupGenerator(&gen, w->mc, 0);

    uint64_t local = 0;
    const uint64_t flush_every = 1ULL << 18;   /* report progress every 256K seeds */

    if (w->mode == MODE_COMMON) {
        for (int64_t s = w->sub_s0_signed; ; s++) {
            uint64_t seed = (uint64_t)s;
            if (phase_a(w->ev, w->n_lower48, seed, w->mc)) {
                if (w->n_biome == 0 ||
                    phase_b(&gen, w->ev, w->nev, w->n_lower48, seed, w->mc))
                {
                    emit(w, seed);
                }
            }
            local++;
            if ((local & (flush_every - 1)) == 0) {
                pthread_mutex_lock(w->prog_mu);
                *w->progress += flush_every;
                pthread_mutex_unlock(w->prog_mu);
            }
            if (s == w->sub_s1_signed) break;
        }
    } else { /* MODE_RANDOM */
        for (uint64_t s48 = w->sub_s0_u48; ; s48++) {
            if (phase_a(w->ev, w->n_lower48, s48, w->mc)) {
                if (w->n_biome > 0) {
                    /* Lift through every value of the upper 16 bits. */
                    for (uint32_t up = 0; up < 65536u; up++) {
                        uint64_t seed = ((uint64_t)up << 48) | s48;
                        if (phase_b(&gen, w->ev, w->nev, w->n_lower48, seed, w->mc))
                            emit(w, seed);
                    }
                } else {
                    /* Without biome evidence, every upper-16 lift matches.
                     * Report the lower-48 candidate; the user will need
                     * biome evidence to pin down the upper 16 bits. */
                    emit(w, s48);
                }
            }
            local++;
            if ((local & (flush_every - 1)) == 0) {
                pthread_mutex_lock(w->prog_mu);
                *w->progress += flush_every;
                pthread_mutex_unlock(w->prog_mu);
            }
            if (s48 == w->sub_s1_u48) break;
        }
    }

    pthread_mutex_lock(w->prog_mu);
    *w->progress += local & (flush_every - 1);
    pthread_mutex_unlock(w->prog_mu);
    return NULL;
}

/* ── Progress reporter (runs in main thread between thread joins) ───── */
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

/* ── Main ───────────────────────────────────────────────────────────── */
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== SeedCrackerZ ===  Java Edition seed cracker (POSIX terminal)\n");
    printf("    by Batthepig  |  powered by cubiomes\n\n");

    /* 1. Minecraft version */
    printf("Minecraft version:\n");
    printf("  1) 1.21   2) 1.20   3) 1.19   4) 1.18   5) 1.17   6) 1.16\n");
    int mvc=prompt_int("Choice",1);
    const int   MCMAP[] = {MC_1_21,MC_1_20,MC_1_19,MC_1_18,MC_1_17,MC_1_16};
    const char *MCNM[]  = {"1.21","1.20","1.19","1.18","1.17","1.16"};
    if (mvc<1||mvc>6) mvc=1;
    int mc=MCMAP[mvc-1];
    const char *mcname=MCNM[mvc-1];

    /* 2. Collect evidence */
    static Ev ev[MAX_EV];
    int nev=0;

    printf("\n-- Evidence Collection -----------------------------------------------\n");
    printf("Commands:  s=structure  c=slime chunk  b=biome  p=print list  d=done\n\n");

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

        if (nev>=MAX_EV) printf("Evidence list full (%d).\n\n",MAX_EV);
        else printf("Unknown command.\n\n");
    }

    /* Re-order evidence: cheap (slime) first, then structure, then biome.
     * Phase A iterates indices [0, n_lower48); Phase B iterates [n_lower48, nev). */
    for (int i=0;i<nev;i++) for (int j=i+1;j<nev;j++) {
        int ci=(ev[i].type==EV_SLIME)?0:(ev[i].type==EV_STRUCT)?1:2;
        int cj=(ev[j].type==EV_SLIME)?0:(ev[j].type==EV_STRUCT)?1:2;
        if (cj<ci) { Ev t=ev[i]; ev[i]=ev[j]; ev[j]=t; }
    }
    int n_struct = 0, n_slime = 0, n_biome = 0;
    for (int i=0;i<nev;i++) {
        if (ev[i].type==EV_STRUCT) n_struct++;
        else if (ev[i].type==EV_SLIME) n_slime++;
        else n_biome++;
    }
    int n_lower48 = n_struct + n_slime;

    /* 3. Search mode */
    printf("\n-- Search Mode -------------------------------------------------------\n");
    printf("  1) Common-seed mode  (signed 32-bit, fast)\n");
    printf("       Hand-typed numeric seeds, hashed string seeds, or any\n");
    printf("       seed that fits in a signed int. Default range:\n");
    printf("       [-2147483648 .. 2147483647]. Typically <1 minute.\n\n");
    printf("  2) Random-seed mode  (unsigned 48-bit, slow)\n");
    printf("       Seeds Minecraft picked itself with new Random().nextLong().\n");
    printf("       Iterates the lower 48 bits, then lifts to 64 bits via\n");
    printf("       biome evidence. Hours to days; needs 2+ structures or\n");
    printf("       3+ slime chunks for tractable runtime.\n\n");
    int mc_choice = prompt_int("Choice", 1);
    SearchMode mode = (mc_choice == 2) ? MODE_RANDOM : MODE_COMMON;

    /* 4. Range */
    int64_t  s0_signed = 0, s1_signed = 0;
    uint64_t s0_u48 = 0,    s1_u48 = 0;
    uint64_t total = 0;

    if (mode == MODE_COMMON) {
        printf("\nSeed range:\n");
        s0_signed = prompt_i64("Range start", -2147483648LL);
        s1_signed = prompt_i64("Range end",    2147483647LL);
        if (s1_signed < s0_signed) { int64_t t = s0_signed; s0_signed = s1_signed; s1_signed = t; }
        total = (uint64_t)(s1_signed - s0_signed) + 1ULL;
    } else {
        printf("\nLower-48 range (unsigned, max %llu):\n",
               (unsigned long long)MASK48);
        s0_u48 = prompt_u64("Range start", 0ULL);
        s1_u48 = prompt_u64("Range end",   MASK48);
        if (s0_u48 > MASK48) s0_u48 = MASK48;
        if (s1_u48 > MASK48) s1_u48 = MASK48;
        if (s1_u48 < s0_u48) { uint64_t t = s0_u48; s0_u48 = s1_u48; s1_u48 = t; }
        total = (s1_u48 - s0_u48) + 1ULL;
    }

    /* 5. Thread count */
    int max_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (max_cores < 1) max_cores = 1;
    if (max_cores > MAX_THREADS) max_cores = MAX_THREADS;
    int n_threads = prompt_int("Threads (1 = single-threaded)", max_cores);
    if (n_threads < 1) n_threads = 1;
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;
    /* Don't spawn more threads than seeds. */
    if ((uint64_t)n_threads > total) n_threads = (int)total;

    printf("\n%d evidence item(s) | %.3f B seeds | Java %s | %d thread(s)\n",
           nev, total/1e9, mcname, n_threads);
    printf("Phase A (lower-48): %d structure + %d slime\n", n_struct, n_slime);
    printf("Phase B (upper-16): %d biome\n\n", n_biome);
    print_ev(ev,nev);
    printf("\n");

    if (mode == MODE_RANDOM && n_lower48 < 2 && total > (1ULL << 36)) {
        printf("WARNING: random-seed mode with fewer than 2 structure/slime constraints\n");
        printf("         over a wide range will take days to weeks. Add evidence or\n");
        printf("         narrow the range with Range start / Range end.\n\n");
    }

    /* 6. Set up shared state. */
    static uint64_t out_buf[MAX_OUT];
    int      nout = 0;
    pthread_mutex_t out_mu;
    pthread_mutex_init(&out_mu, NULL);

    uint64_t progress = 0;
    pthread_mutex_t prog_mu;
    pthread_mutex_init(&prog_mu, NULL);

    /* 7. Partition the range across threads and launch. */
    Worker  workers[MAX_THREADS];
    pthread_t tids[MAX_THREADS];

    uint64_t per = total / (uint64_t)n_threads;
    uint64_t rem = total - per * (uint64_t)n_threads;

    if (mode == MODE_COMMON) {
        int64_t cur = s0_signed;
        for (int i = 0; i < n_threads; i++) {
            uint64_t span = per + (i < (int)rem ? 1 : 0);
            workers[i] = (Worker){
                .sub_s0_signed = cur,
                .sub_s1_signed = cur + (int64_t)(span - 1),
                .mode = mode, .mc = mc,
                .ev = ev, .nev = nev,
                .n_lower48 = n_lower48, .n_biome = n_biome,
                .out_mu = &out_mu, .out = out_buf, .nout = &nout, .ocap = MAX_OUT,
                .prog_mu = &prog_mu, .progress = &progress,
            };
            cur += (int64_t)span;
        }
    } else {
        uint64_t cur = s0_u48;
        for (int i = 0; i < n_threads; i++) {
            uint64_t span = per + (i < (int)rem ? 1 : 0);
            workers[i] = (Worker){
                .sub_s0_u48 = cur,
                .sub_s1_u48 = cur + span - 1,
                .mode = mode, .mc = mc,
                .ev = ev, .nev = nev,
                .n_lower48 = n_lower48, .n_biome = n_biome,
                .out_mu = &out_mu, .out = out_buf, .nout = &nout, .ocap = MAX_OUT,
                .prog_mu = &prog_mu, .progress = &progress,
            };
            cur += span;
        }
    }

    double t0 = now_sec();
    for (int i = 0; i < n_threads; i++)
        pthread_create(&tids[i], NULL, worker_main, &workers[i]);

    /* 8. Live progress while workers run. With the inline-pthread shim
     * (iOS), workers complete during pthread_create above, so this loop
     * sees progress == total immediately and exits after one tick. */
    for (;;) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&prog_mu);
        uint64_t done = progress;
        pthread_mutex_unlock(&prog_mu);

        pthread_mutex_lock(&out_mu);
        int found = nout;
        pthread_mutex_unlock(&out_mu);

        double el = now_sec() - t0;
        double frac = total ? (double)done / (double)total : 1.0;
        double speed_Mps = el > 0.0 ? (double)done / el / 1e6 : 0.0;
        print_bar(frac, el, found, speed_Mps);

        if (done >= total) break;
    }

    for (int i = 0; i < n_threads; i++)
        pthread_join(tids[i], NULL);

    double dt = now_sec() - t0;
    /* Final 100% line. */
    print_bar(1.0, dt, nout, dt > 0.0 ? (double)total / dt / 1e6 : 0.0);
    printf("\n\nFinished in %.2fs.\n\n", dt);

    pthread_mutex_destroy(&out_mu);
    pthread_mutex_destroy(&prog_mu);

    /* 9. Results */
    if (!nout) {
        printf("No seeds matched.\n\n");
        printf("Troubleshooting:\n");
        printf("  1. Re-read F3 coordinates -- one wrong digit rules out the real seed.\n");
        printf("  2. Confirm Minecraft version matches when the world was created.\n");
        printf("  3. For structures: try +-16 blocks (one chunk) in each direction.\n");
        printf("     You may not be standing in the origin chunk.\n");
        printf("  4. For slime chunks: was your test below Y=40 in a cave, not swamp?\n");
        printf("     Swamp slimes spawn everywhere and don't follow the chunk rule.\n");
        printf("  5. If you used Common-seed mode and your world was created without\n");
        printf("     a custom seed, switch to Random-seed mode (option 2).\n");
    } else if (mode == MODE_RANDOM && n_biome == 0) {
        printf("Matching lower-48-bit seed value(s):\n\n");
        for (int i=0;i<nout;i++) printf("  %llu\n", (unsigned long long)out_buf[i]);
        printf("\nNo biome evidence was provided, so the upper 16 bits of the world\n");
        printf("seed cannot be determined. Each lower-48 value above corresponds to\n");
        printf("65536 possible 64-bit world seeds. Add a biome sample and re-run\n");
        printf("to pin down the exact world seed.\n");
    } else {
        printf("Matching seed(s):\n\n");
        for (int i=0;i<nout;i++) printf("  %lld\n", (long long)out_buf[i]);
        printf("\n");
        if (nout == 1)
            printf("One match -- almost certainly your world seed. Verify with /seed in-game.\n");
        else {
            printf("%d candidates. To narrow down:\n",nout);
            printf("  - Add a biome observation (most effective filter).\n");
            printf("  - Add more slime-chunk or structure evidence.\n");
        }
        if (nout >= MAX_OUT)
            printf("\n(output buffer was capped at %d -- there may be more matches)\n",
                   MAX_OUT);
    }

    return 0;
}
