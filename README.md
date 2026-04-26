# SeedCrackerZ

A Java Edition Minecraft world seed cracker for any POSIX terminal.
Provide observable evidence from your world — structure locations, biomes,
slime chunks — and it searches a seed range to find which seed matches.

Powered by the [cubiomes](https://github.com/cubitect/cubiomes) library,
which implements Mojang's exact world-generation algorithms in C.

Built by **Batthepig**.

> **SeedCrackerX runs inside Minecraft as a mod.**
> **SeedCrackerZ runs outside Minecraft in a terminal — no mod, no install.**

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Build](#build)
3. [Run](#run)
4. [Evidence Types](#evidence-types)
   - [Structure Location](#structure-location)
   - [Buried Treasure](#buried-treasure)
   - [Slime Chunk](#slime-chunk)
   - [Biome Sample](#biome-sample)
   - [World Spawn](#world-spawn)
5. [Step-by-Step Usage](#step-by-step-usage)
6. [Common-Seed vs Random-Seed Mode](#common-seed-vs-random-seed-mode)
7. [Evidence Quality Guide](#evidence-quality-guide)
8. [Resume After Interruption](#resume-after-interruption)
9. [Understanding Results](#understanding-results)
10. [Troubleshooting](#troubleshooting)
11. [Platform Notes](#platform-notes)

---

## Prerequisites

You need a C compiler and `curl`. Nothing else — `make` handles the rest.

| Platform | One-time setup |
|---|---|
| Ubuntu / Debian | `sudo apt install clang make curl` |
| macOS | `xcode-select --install` |
| a-Shell (iPhone / iPad) | nothing — `clang`, `make`, and `curl` are built in |
| Windows | install [WSL](https://learn.microsoft.com/en-us/windows/wsl/), then follow the Ubuntu line |

`git` is **not** required. The Makefile downloads cubiomes (and its data
tables) over plain HTTPS using `curl`.

---

## Build

```sh
curl -O https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main/seedcrackerz.c
curl -O https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main/Makefile
make
```

`make` downloads cubiomes and compiles everything in one step. Re-running
`make` is a no-op once the binary exists. Use `make clean` to wipe just the
binary, or `make distclean` to also remove the downloaded cubiomes folder.

---

## Run

```sh
./seedcrackerz
```

The program is fully interactive. It asks for your Minecraft version, then
lets you add as many evidence items as you have before running the search.

---

## Evidence Types

### Structure Location

Tell SeedCrackerZ that a specific structure exists at a location you visited.

**How to use:**
1. Walk to the exact position listed in the table below for that structure.
2. Open F3 and read your **X** and **Z** block coordinates.
3. Enter them when prompted — the tool converts block → chunk automatically.

| Structure | Where to stand |
|---|---|
| Village | At the centre well or main road junction |
| Desert Pyramid | In the pressure-plate trap room (centre of ground floor) |
| Jungle Temple | In the ground-floor centre room |
| Swamp Hut | On the hut floor |
| Igloo | In the centre of the igloo floor |
| Pillager Outpost | At the base of the tower |
| Ocean Monument | In the Elder Guardian's central chamber |
| Woodland Mansion | At the entrance doorway |
| Nether Fortress | In the first corridor near the entrance arch |
| Bastion Remnant | In the centre of the courtyard |
| Shipwreck | On the highest deck |

**If no seed matches:** the block you recorded might not be the origin chunk.
Try shifting by ±16 blocks (one chunk) in each direction and re-running.

---

### Buried Treasure

Buried treasure chests are placed by Minecraft on a per-chunk dice roll
that depends on the lower 48 bits of the world seed. Telling SeedCrackerZ
"a chunk DOES contain buried treasure" eliminates roughly 99% of candidate
seeds, so even one treasure chest is a powerful Phase-A constraint.

**How to use:**
1. While playing, dig up a buried-treasure chest using a treasure map
   (or by exploring beach chunks).
2. Stand on the chest and read **X** and **Z** from F3.
3. In the evidence menu, pick `s` (structure), then choose
   `Buried_Treasure` from the structure list.
4. Enter your block X and Z — the tool converts to chunk coordinates
   automatically.

A single buried-treasure chunk on its own typically narrows a 32-bit scan
to a few dozen candidates; combining it with one other piece of evidence
(another chest, a structure, a slime chunk, or a spawn reading) usually
collapses to a single seed.

---

### Slime Chunk

Slime chunks spawn underground slimes at any Y below 40, in any biome.
They are determined solely by the world seed and chunk coordinates — making
them one of the fastest and most powerful cracking inputs.

**How to test:**
1. Dig a room at Y 30–35 in the chunk you want to test.
   (**F3+G** shows chunk boundaries as coloured grid lines.)
2. Wait two Minecraft nights (~10 real minutes without sleeping).
3. Slimes spawn → IS a slime chunk. None appear → NOT.

> **Note:** Swamp slimes spawn above ground everywhere regardless of seed.
> Only underground slimes (in caves, below Y=40) follow the slime-chunk rule.

**Chunk coordinates:** the F3 screen shows a `Chunk:` line whose third
group of numbers is the absolute chunk X and Z. Or: chunk X = floor(block X ÷ 16).

---

### Biome Sample

Tell the tool what biome is at a specific block location.

- Stand anywhere in the target biome.
- Read X, Z from F3.
- Enter the biome name when prompted.

Biome checks run the full cubiomes biome generator and are slower per seed.
The tool sorts evidence cheapest-first automatically, so biome checks only
run on seeds that already passed all slime, structure and treasure checks.

---

### World Spawn

The block where you respawn after dying without a bed (the world's
"compass" point) is uniquely determined by the **full 64-bit** seed.
Spawn evidence is therefore a Phase-B filter: it lifts a lower-48
candidate to the full 64-bit seed in random-seed mode, just like a
biome sample does.

**How to find your world spawn:**

| Method | Steps |
|---|---|
| Sleep + break a bed | Sleep in any bed, then break it. The next time you respawn, you appear at world spawn. Read X / Z from F3. |
| Operator command | If you have op (cheats), run `/spawnpoint` with no arguments and it prints the world-spawn block. |
| F3 readout | New worlds: spawn at the location you started before you moved. Read X / Z from F3 immediately on world creation. |

**Tolerance:** the spawn algorithm picks a "fittest" block within a
small region, so a few blocks of slack helps if your reading is slightly
off. The default tolerance is 16 blocks (1 chunk); raise it to 32 or 64
if you're not sure of the exact spawn block.

> Spawn evidence is the most powerful single piece of Phase-B evidence
> you can give SeedCrackerZ. Combined with one good Phase-A item (a
> structure, a buried-treasure chunk, or a few slime chunks) it
> typically collapses a search to a single matching seed.

---

## Step-by-Step Usage

```
Minecraft version:
  1) 1.21   2) 1.20   3) 1.19   4) 1.18   5) 1.17   6) 1.16
Choice [1]:
```

Choose the version the world was **created** in (structure algorithms change
between major versions).

```
Add evidence:  s=structure   c=slime chunk   b=biome
               w=world spawn  p=print list    d=done
```

Add items one at a time. Use `p` to review your list. Type `d` when done.

> Buried treasure is one of the structure types listed under `s` —
> pick `Buried_Treasure` from the structure menu.

```
-- Search Mode --
  1) Common-seed mode  (signed 32-bit, fast)
  2) Random-seed mode  (unsigned 48-bit, slow)
Choice [1]:
```

See [Common-Seed vs Random-Seed Mode](#common-seed-vs-random-seed-mode) for
which to pick.

```
Range start [-2147483648]:
Range end   [2147483647]:
Threads (1 = single-threaded) [8]:
```

The thread count defaults to the number of CPU cores detected (capped at 16).
Set it to 1 if you want a single worker, or higher for more parallelism.

---

## Common-Seed vs Random-Seed Mode

Java's RNG is internally 48-bit, so structure placement, slime chunks
and buried-treasure presence depend only on the **lower 48 bits** of the
world seed. Biome generation and world-spawn placement use the **full
64 bits**. SeedCrackerZ exploits this with a two-phase search:

- **Phase A (lower 48):** structure + slime + buried-treasure checks —
  these run on every candidate.
- **Phase B (upper 16):** biome and world-spawn checks — these only run
  on candidates that already passed Phase A. When Phase-B evidence is
  present, the search *lifts* each Phase-A survivor to a 64-bit seed by
  trying every value of the upper 16 bits (65 536 lifts per candidate).

You pick which slice of the seed space to scan:

| Mode | Range scanned | Best for |
|---|---|---|
| **Common-seed** (option 1) | signed 32-bit `[-2³¹, 2³¹-1]` | Numeric or string seeds typed by hand. Hashed strings and numeric seeds both fit in a signed int and get sign-extended to 64 bits, so a 32-bit scan with no lifting reaches them all. |
| **Random-seed** (option 2) | unsigned 48-bit `[0, 2⁴⁸-1]` | Seeds Minecraft picked itself with `new Random().nextLong()` — these have random upper-16 bits and live anywhere in the 64-bit space. Phase B lifts each Phase-A match to recover the upper 16. |

**If you used random-seed mode without any Phase-B evidence** (no biome
samples, no spawn reading), the cracker will report the *lower-48
candidate values* it found. Each one represents 65 536 possible 64-bit
world seeds; you need at least one biome sample or one spawn reading to
narrow it down to a single seed.

---

## Evidence Quality Guide

| Evidence combination | Mode | Typical time |
|---|---|---|
| 2+ different structure types | Common-seed (32-bit) | ~15 seconds |
| 1 structure + 1 biome | Common-seed (32-bit) | ~30 seconds |
| 1 structure + 1 spawn reading | Common-seed (32-bit) | ~30 seconds, almost always unique |
| 1 buried-treasure chunk + any other Phase-A | Common-seed (32-bit) | ~10 seconds, almost always unique |
| 3+ slime chunks | Common-seed (32-bit) | ~5 seconds |
| 2+ structures + 1 biome | Random-seed (48-bit) | hours on multi-core |
| 3+ slime chunks + 1 structure + 1 spawn | Random-seed (48-bit) | hours on multi-core, usually unique |
| 1 structure only | Random-seed (48-bit) | days — add more evidence first |
| Biome observations only | either mode | many false positives |

**Tips:**
- Two different structure types from different areas of your world is the
  single best starting combination.
- A buried-treasure chunk is the strongest single Phase-A item — pair one
  with any other piece of evidence and the result is usually a single seed.
- Each slime chunk eliminates ~90% of remaining candidates — three is usually
  enough to wipe out all false positives.
- A spawn-point reading is the strongest single Phase-B item. Pair it with
  any one Phase-A item to almost always get a unique 64-bit seed.
- Random-seed mode without 2+ Phase-A constraints (structures / slime /
  treasure) is impractical — the inner loop has to scan 2⁴⁸ ≈ 281
  trillion values.

---

## Resume After Interruption

Long random-seed runs can take hours. SeedCrackerZ writes a checkpoint
file (`seedcrackerz.checkpoint`, in the working directory) every few
seconds. If the run is interrupted — Ctrl+C, terminal closed, phone
locked, battery dying — re-run the cracker and it will detect the
checkpoint:

```
Found a saved checkpoint:
  Mode:         Random-seed (48-bit)
  Minecraft:    1.20
  Range:        0 .. 281474976710655 (lower-48)
  Evidence:     3 item(s)
  Progress:     45000000000 / 281474976710655  (16.0%)
  Matches saved: 0
Resume this run? (y/n) [y]:
```

Press **Enter** (or `y`) to pick up where you left off, or `n` to discard
the checkpoint and start a new search. The checkpoint is deleted
automatically when a run completes normally.

You can change the thread count between sessions — the work-distribution
is dynamic, not pinned to a fixed number of workers, so a run started
with 8 threads on a desktop can be resumed with 1 thread on a phone (or
the reverse).

---

## Understanding Results

```
Matching seed(s):

  -27494042902267137001
```

Each listed seed produces every piece of evidence you provided.
Verify with `/seed` in-game (requires cheats or operator permission).

**One result** — almost certainly your world seed.

**Multiple results** — add a biome sample or another slime chunk and run again.

**No results** — see troubleshooting.

---

## Troubleshooting

**No seeds found:**

1. Re-read your F3 coordinates — one wrong digit rules out the real seed.
2. Confirm the Minecraft version matches when the world was created.
3. For structures: try ±16 blocks (one chunk) in each direction.
4. For slime chunks: was your test below Y=40 in a **cave**, not swamp ground?
5. If you used common-seed mode (option 1) and your world was created
   without a typed seed, switch to random-seed mode (option 2).

**Too many results:**

- Add a world-spawn reading — strongest single Phase-B filter.
- Add a buried-treasure chunk — strongest single Phase-A filter.
- Add a biome observation.
- Add more slime-chunk results (each one cuts ~90%).
- Add a second structure of a different type.

**Run was interrupted, want to start fresh:**

Delete the checkpoint file, then run again:

```sh
rm seedcrackerz.checkpoint
```

```sh
./seedcrackerz
```

(Or just answer `n` when the cracker asks "Resume this run?")

**Build errors:**

| Error | Fix |
|---|---|
| `cc: command not found` | The current `Makefile` auto-detects `clang` when `cc` is missing; if you still see this you have an old `Makefile` — re-`curl` it. |
| `make: don't know how to make \|. Stop` | Old `Makefile` parsed by BSD make (a-Shell, FreeBSD). Re-`curl` the current `Makefile` from this repo — it's now bmake-compatible. |
| `pthread.h: file not found` | Already handled — the build ships a fallback in `compat/`. If you still see this, make sure you pulled the latest `Makefile`. |
| `tables/btreeXX.h: file not found` | Run `make distclean`, then `make`, to redownload cubiomes cleanly. |
| `Graph cycles through cubiomes/...` | Old Makefile. Re-download the current `Makefile` from this repo. |
| `curl: (6) Could not resolve host` | No internet. The first `make` needs to download cubiomes. |

---

## Platform Notes

### a-Shell (iPhone / iPad)

a-Shell ships everything you need — `clang`, `make`, and `curl`. There is
no separate setup step.

> **a-Shell does not understand `&&`** — if you type
> `mkdir foo && cd foo`, it will create literal folders called `foo`,
> `&&`, and `cd`, then complain. Run each command on its own line, exactly
> as written below.

```sh
cd ~/Documents
```

```sh
mkdir seedcrackerz
```

```sh
cd seedcrackerz
```

```sh
curl -O https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main/seedcrackerz.c
```

```sh
curl -O https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main/Makefile
```

```sh
make
```

```sh
./seedcrackerz
```

If `make` complains that `cc` isn't found, run it as `make CC=clang`
instead. The build automatically uses a bundled `pthread.h` shim that
runs worker threads inline, so SeedCrackerZ compiles and runs correctly
on iOS even though a-Shell ships no pthread header. The search runs on
a single core there; results are identical to a multi-core run, just
slower.

a-Shell is sandboxed to `~/Documents`, so keep the project there.

### macOS

`xcode-select --install` provides clang. `make` and `curl` are already
included.

### Windows (WSL)

1. Open PowerShell as Administrator: `wsl --install`, then restart.
2. Open the Ubuntu app: `sudo apt install clang make curl`.
3. Follow the Build steps above.
