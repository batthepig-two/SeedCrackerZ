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
   - [Slime Chunk](#slime-chunk)
   - [Biome Sample](#biome-sample)
5. [Step-by-Step Usage](#step-by-step-usage)
6. [Evidence Quality Guide](#evidence-quality-guide)
7. [Understanding Results](#understanding-results)
8. [Troubleshooting](#troubleshooting)
9. [Platform Notes](#platform-notes)

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
run on seeds that already passed all slime and structure checks.

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
Add evidence:  s=structure  c=slime chunk  b=biome  p=print list  d=done
```

Add items one at a time. Use `p` to review your list. Type `d` when done.

```
Seed range start [-2147483648]:
Seed range end   [2147483647]:
```

The default (full 32-bit signed integer range) takes 15–30 seconds at
100–300 million seeds/second. See the quality guide for wider ranges.

---

## Evidence Quality Guide

| Evidence combination | Effective range | Typical time |
|---|---|---|
| 2+ different structure types | 32-bit default | ~15 seconds |
| 1 structure + 1 biome | 32-bit default | ~30 seconds |
| 3+ slime chunks | 32-bit default | ~5 seconds |
| 3+ slime chunks + 1 structure | 48-bit range | 30–90 minutes |
| Biome observations only | 32-bit default | Minutes, many false positives |

**Tips:**
- Two different structure types from different areas of your world is the
  single best starting combination.
- Each slime chunk eliminates ~90% of remaining candidates — three is usually
  enough to wipe out all false positives.
- Add a biome sample last to eliminate any remaining ambiguity.

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
5. Widen the search range — your seed may be outside the 32-bit default.

**Too many results:**

- Add a biome observation — most effective false-positive filter.
- Add more slime-chunk results (each one cuts ~90%).
- Add a second structure of a different type.

**Build errors:**

| Error | Fix |
|---|---|
| `cc: command not found` | Use `make CC=clang` (or install `cc`). |
| `pthread.h: file not found` | Already handled — the build ships a fallback in `compat/`. If you still see this, make sure you pulled the latest `Makefile`. |
| `tables/btreeXX.h: file not found` | Run `make distclean && make` to redownload cubiomes cleanly. |
| `Graph cycles through cubiomes/...` | Old Makefile. Re-download the current `Makefile` from this repo. |
| `curl: (6) Could not resolve host` | No internet. The first `make` needs to download cubiomes. |

---

## Platform Notes

### a-Shell (iPhone / iPad)

a-Shell ships everything you need — `clang`, `make`, and `curl`. There is
no separate setup step.

```sh
cd ~/Documents
mkdir seedcrackerz && cd seedcrackerz
curl -O https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main/seedcrackerz.c
curl -O https://raw.githubusercontent.com/batthepig-two/SeedCrackerZ/main/Makefile
make
./seedcrackerz
```

If `make` complains that `cc` isn't found, run it as `make CC=clang`
instead. The build automatically uses a bundled `pthread.h` shim, so the
parallel quad-feature search compiles and runs single-threaded on iOS.

a-Shell is sandboxed to `~/Documents`, so keep the project there.

### macOS

`xcode-select --install` provides clang. `make` and `curl` are already
included.

### Windows (WSL)

1. Open PowerShell as Administrator: `wsl --install`, then restart.
2. Open the Ubuntu app: `sudo apt install clang make curl`.
3. Follow the Build steps above.
