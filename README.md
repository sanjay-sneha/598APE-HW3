# Artifact for Abelian Sandpile Optimizations

This explains how to build the code, reproduce the baseline and optimized results, and evaluate each individual optimization. Every optimization is its own commit on the `main` branch, on top of the original code.

All results in the report were measured on the course VM (4 cores). Everything here runs on a standard Unix machine; no special hardware is required.

---

## 1. Tools and requirements

- `gcc` with OpenMP support (OpenMP's `libgomp` ships with GCC)
- `make`
- `perf` (for profiling and hardware counters)
- [FlameGraph](https://github.com/brendangregg/FlameGraph) scripts (to generate flamegraphs)
- `python3` with `matplotlib` (to regenerate the report's tables and graph)

`docker/Dockerfile` installs these. On Ubuntu without Docker:

```bash
sudo apt install -y build-essential linux-tools-common linux-tools-$(uname -r) python3-matplotlib
```

---

## 2. Building and running

### Build and run

```bash
make clean && make all
time ./sandpile.exe 1001
```

This writes the identity sandpile to `identity.png`. The numbers in the report are the `real` times from `time`. The final version takes about 12 s on the course VM; the baseline takes about 11 minutes.

OpenMP uses all available cores by default. To pin the thread count, you can set it explicitly, with `OMP_NUM_THREADS=4 ./sandpile.exe 1001`.

### Run the baseline

The baseline is the first commit (the original code). To build it next to the optimized version without switching branches:

```bash
git log --oneline                                     # to check the bottom commit is the baseline
git show 15fe837:sandpile.c > baseline_sandpile.c
gcc baseline_sandpile.c -o baseline.exe -O3 -lm -g -Werror
time ./baseline.exe 1001                              # ~11 minutes on the course VM
mv identity.png baseline_1001.png
```

### Verify correctness

Every optimization produces a byte-identical `identity.png` to the baseline. After running the optimized version:

```bash
./sandpile.exe 1001
diff identity.png baseline_1001.png                   # prints nothing if identical
```

For a quicker check, compare at a smaller odd size, e.g. `N=401` (the baseline takes about 17s for this):

```bash
./baseline.exe 401 && mv identity.png baseline_401.png
./sandpile.exe 401 && diff identity.png baseline_401.png
```

The output stays identical even though later optimizations change the order of topplings, because of the abelian property where any legal toppling sequence reaches the same stable grid with the same number of topples per cell.

### Limits of the final version

- **Symmetry** (optimization 6) only speeds up odd `N`. Even `N` is still correct but it just simulates every row.
- **16-bit cells** (optimization 7) are only safe up to `N = 2001`. Larger `N` exits with a message instead of producing a wrong image. To run larger grids, change `typedef int16_t cell_t;` to `typedef int32_t cell_t;` in `sandpile.c` and rebuild.
- **`-march=native`** (optimization 8) tunes the binary for the machine it is built on, so build it on the machine you run it on. The source still builds on machines without AVX2.

---

## 3. Profiling with perf

### Hardware counters

The counter tables in the report use `perf stat` at `N=401`, which takes about 2.6% of the `N=1001` time and gives the same ratios between versions:

```bash
perf stat -e instructions,cycles,branches,branch-misses,cache-misses ./sandpile.exe 401
```

From optimization 5 and beyond, the counts are summed over all threads.

### Flamegraph

The flamegraph build adds frame pointers so `perf` gets readable call stacks (the timing builds use the unmodified flags):

```bash
git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph   # once

make clean && make all FLAGS="-O3 -lm -g -Werror -fopenmp -march=native -fno-omit-frame-pointer"
perf record -F 999 -g ./sandpile.exe 1001
perf script | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl --title "Flamegraph" > flamegraph.svg
```

Open `flamegraph.svg` in a browser. For the baseline flamegraph in the report, you can use the baseline commit and drop `-fopenmp -march=native` from `FLAGS`.

### Regenerating the report's tables and graph

Save each version's timing and counter output as `<name>_time.txt` and `<name>_stat.txt` (the names are listed at the top of `make_tables.py`), then:

```bash
python3 make_tables.py
```

This writes the summary runtime table, the per-optimization counter tables, and `runtime_graph.pdf`.

---

## 4. Optimizations and how to evaluate

Each optimization is its own commit. To evaluate one, check out its commit, rebuild, and time it; then do the same for the commit before it. The difference between the two times reproduces the result reported in that section of the paper.

```bash
git checkout <commit>
make clean && make all
time ./sandpile.exe 1001
diff identity.png baseline_1001.png
git checkout main          # return to the latest version when done
```

| # | Optimization | Commit | Time at N=1001 (course VM) |
|---|---|---|---|
| 0 | Baseline (original course code) | `15fe837` | 657.3 s |
| 1 | Precompute per-row shape bounds instead of a per-cell circle test | `7d4e1cd` | 312.0 s |
| 2 | Branchless topple: topple `grid >> 2` times per visit, no `grid < 4` branch | `cfd4b14` | 300.3 s |
| 3 | Flat padded array (one contiguous block with a zero border) | `e8924f5` | 281.1 s |
| 4 | Two-pass update (no cell-to-cell dependency, so gcc vectorizes it) | `683d83e` | 221.5 s |
| 5 | OpenMP across rows (adds `-fopenmp` to the `Makefile`) | `4a322a0` | 63.4 s |
| 6 | Top-to-bottom mirror symmetry (simulate half the rows, odd `N`) | `81210f8` | 28.8 s |
| 7 | 16-bit cells (`int16_t`) | `407d465` | 15.3 s |
| 8 | `-march=native` (adds the flag to the `Makefile`) | `a47b203` | 11.6 s |

Optimizations 5 and 8 change the `Makefile`; all others are source changes in `sandpile.c`. The absolute times can be expected to differ on other machines, especially from optimization 5 on, which depends on the number of cores.
