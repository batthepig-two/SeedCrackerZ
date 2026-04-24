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
 * ── Evidence quality guide ────────────────────────────────────────────────
 *   Fastest (seconds on default 32-bit range):
 *     2+ different structure types
 *   Good (seconds to minutes):
 *     1 structure + 1 biome sample   OR   3+ slime-chunk results
 *   Slow (hours for 48-bit range):
 *     slime chunks alone with a very wide range
 * ────────────────────────────────────────────────────────────────────────
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "finders.h"
#include "generator.h"
#include "biomes.h"

/* ── Constants ──────────────────────────────────────────────────────── */
#define MAX_EV  64
#define MASK48  ((INT64_C(1) << 48) - 1)

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

/* ── Slime chunk check ──────────────────────────────────────────────── */
/*
 * Matches Java Edition exactly:
 *   new Random(seed
 *     + (long)(cx*cx*0x4c1906) + (long)(cx*0x5ac0db)
 *     + (long)(cz*cz)*0x4307a7L + (long)(cz*0x5f24f)
 *     ^ 0x3ad8025fL).nextInt(10) == 0
 *
 * The (long)(int32 expr) casts replicate Java int32 overflow then widen.
 */
static bool check_slime(int64_t seed, int cx, int cz)
{
    int64_t a = (int64_t)(int32_t)((uint32_t)cx*(uint32_t)cx*(uint32_t)0x4c1906u);
    int64_t b = (int64_t)(int32_t)((uint32_t)cx*(uint32_t)0x5ac0dbu);
    int64_t c = (int64_t)(int32_t)((uint32_t)cz*(uint32_t)cz) * (int64_t)0x4307a7;
    int64_t d = (int64_t)(int32_t)((uint32_t)cz*(uint32_t)0x5f24fu);
    int64_t sv  = (seed + a + b + c + d) ^ (int64_t)0x3ad8025f;
    int64_t st  = (sv ^ INT64_C(0x5DEECE66D)) & MASK48;
    st = (st * INT64_C(0x5DEECE66D) + 0xBLL) & MASK48;
    /* nextInt(10): rejection-sampling probability ~4e-9, safe to ignore */
    return (int)((st >> 17) * 10 >> 31) == 0;
}

/* ── Structure check ────────────────────────────────────────────────── */
static bool check_struct(int stype, int cx, int cz, uint64_t seed, int mc)
{
    StructureConfig sc;
    if (!getStructureConfig(stype, mc, &sc))
        return true;   /* not valid for this MC version — skip */

    /* Floor division for negative chunk coords */
    int regX = (cx < 0 && cx % sc.regionSize) ? cx/sc.regionSize - 1 : cx/sc.regionSize;
    int regZ = (cz < 0 && cz % sc.regionSize) ? cz/sc.regionSize - 1 : cz/sc.regionSize;

    Pos pos;
    if (!getStructurePos(stype, mc, seed, regX, regZ, &pos))
        return false;  /* blocked by biome or other condition */

    return pos.x == cx && pos.z == cz;
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

    /* 2. Set up cubiomes generator (once, reused per candidate seed) */
    Generator gen;
    setupGenerator(&gen, mc, 0);

    /* 3. Collect evidence */
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

        /* structure */
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

        /* slime chunk */
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

        /* biome */
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

    /* Sort evidence: slime (cheapest) first, biome (most expensive) last */
    for (int i=0;i<nev;i++) for (int j=i+1;j<nev;j++) {
        int ci=(ev[i].type==EV_SLIME)?0:(ev[i].type==EV_STRUCT)?1:2;
        int cj=(ev[j].type==EV_SLIME)?0:(ev[j].type==EV_STRUCT)?1:2;
        if (cj<ci) { Ev t=ev[i]; ev[i]=ev[j]; ev[j]=t; }
    }

    /* 4. Seed range */
    printf("\n-- Seed Search Range -------------------------------------------------\n");
    printf("At 100-300 M seeds/sec, the 32-bit default range takes ~15-30 seconds.\n");
    printf("The full 48-bit random-seed space takes many hours on CPU.\n\n");
    int64_t s0=prompt_i64("Seed range start",-2147483648LL);
    int64_t s1=prompt_i64("Seed range end",   2147483647LL);
    if (s1<s0) { int64_t t=s0; s0=s1; s1=t; }
    uint64_t total=(uint64_t)(s1-s0)+1ULL;

    printf("\n%d evidence item(s) | %.3f B seeds | Java Edition %s\n\n",
           nev, total/1e9, mcname);
    print_ev(ev,nev);
    printf("\n");

    /* 5. Search */
    int ocap=512;
    int64_t *out=malloc((size_t)ocap*sizeof(int64_t));
    int nout=0;
    double t0=now_sec();
    uint64_t scanned=0, step=(total>400)?total/400:1, next_chk;
    if (step<500000) step=500000;
    next_chk=step;
    double last_print=t0;

    for (int64_t seed=s0;;seed++) {
        uint64_t us=(uint64_t)seed;
        bool ok=true;
        for (int i=0;i<nev&&ok;i++) {
            const Ev *e=&ev[i];
            switch (e->type) {
            case EV_SLIME:
                if (check_slime(seed,e->sl.cx,e->sl.cz)!=e->sl.yes) ok=false;
                break;
            case EV_STRUCT:
                if (!check_struct(e->st.stype,e->st.cx,e->st.cz,us,mc)) ok=false;
                break;
            case EV_BIOME: {
                applySeed(&gen,DIM_OVERWORLD,us);
                int bm;
                /* 1.18+ has 3-D biomes at 4-block resolution */
                if (mc>=MC_1_18)
                    bm=getBiomeAt(&gen,4,e->bm.bx>>2,16,e->bm.bz>>2);
                else
                    bm=getBiomeAt(&gen,1,e->bm.bx,0,e->bm.bz);
                if (bm!=e->bm.biome) ok=false;
                break;
            }
            }
        }
        if (ok) {
            if (nout==ocap) {
                ocap*=2;
                out=realloc(out,(size_t)ocap*sizeof(int64_t));
            }
            out[nout++]=seed;
        }
        scanned++;
        if (scanned>=next_chk||seed==s1) {
            next_chk=scanned+step;
            double now=now_sec();
            if (now-last_print>=0.2||seed==s1) {
                last_print=now;
                double el=now-t0, frac=(double)scanned/total;
                if (frac>1.0) frac=1.0;
                double eta=frac>0.0?el*(1.0/frac-1.0):0.0;
                int pct=(int)(frac*100+0.5);
                int bars=(int)(frac*30+0.5);
                char bar[32];
                for (int b=0;b<30;b++) bar[b]=b<bars?'#':'-';
                bar[30]=0;
                printf("\r  [%s] %3d%%  %6.1fs  eta %6.1fs  found %-5d",
                       bar,pct,el,eta,nout);
                fflush(stdout);
            }
        }
        if (seed==s1) break;
    }

    double dt=now_sec()-t0;
    printf("\r  [##############################] 100%%  %6.1fs  eta    0.0s  found %-5d\n\n",
           dt,nout);
    printf("Finished in %.2fs  (%.2f M seeds/sec)\n\n",
           dt, dt>0.0?(double)scanned/dt/1e6:0.0);

    /* 6. Results */
    if (!nout) {
        printf("No seeds matched in [%lld, %lld].\n\n",
               (long long)s0,(long long)s1);
        printf("Troubleshooting:\n");
        printf("  1. Re-read F3 coordinates -- one wrong digit rules out the real seed.\n");
        printf("  2. Confirm Minecraft version matches when the world was created.\n");
        printf("  3. For structures: try +-16 blocks (one chunk) in each direction.\n");
        printf("     You may not be standing in the origin chunk.\n");
        printf("  4. For slime chunks: was your test below Y=40 in a cave, not swamp ground?\n");
        printf("     Swamp slimes spawn everywhere and don't follow the chunk rule.\n");
        printf("  5. Widen the range -- your seed may be outside the searched range.\n");
    } else {
        printf("Matching seed(s):\n\n");
        for (int i=0;i<nout;i++) printf("  %lld\n",(long long)out[i]);
        printf("\n");
        if (nout==1)
            printf("One match -- almost certainly your world seed. Verify with /seed in-game.\n");
        else {
            printf("%d candidates. To narrow down:\n",nout);
            printf("  - Add a biome observation (most effective filter).\n");
            printf("  - Add more slime-chunk or structure evidence.\n");
        }
    }

    free(out);
    return 0;
}
