#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

static void die(const char *s) {
  fprintf(stderr, "%s\n", s);
  exit(1);
}

int N = 801;
int* grid; // CHANGED: a contiguous block of (N+2)*(N+2) ints

// ADDED:
// grid has a 1-cell border of padding on every side, so cell (i, j) of the
// N x N sandpile lives at row i+1, column j+1 of the padded grid
// grains that fall off the edge land in the border, which is never rendered,
// so this works for disappearing grains that fall off
#define IDX(i, j) (((i) + 1) * (N + 2) + ((j) + 1))

// ADDED: topple count per cell for the current sweep, same padded layout as grid
// only cells inside the shape are ever written, so every other entry stays 0
static int* tbuf;

// ADDED: per-row shape bound pointers
static int* row_low;
static int* row_high;
// ADDED: per-row range of cells that can receive grains (shape cells + their neighbors)
static int* recv_low;
static int* recv_high;

// ADDED: precompute shape bounds
static void compute_shape(void) {
  row_low = (int*) malloc(N * sizeof(int));
  row_high = (int*) malloc(N * sizeof(int));
  if (!row_low || !row_high)
    die("Malloc failed for shape bounds");
  int c = N / 2;
  for (int i = 0; i < N; ++i) {
    int low = -1, high = -1; // empty row by default
    int jmax = MAX(i, N - 1 - i); // mouth cutoff
    for (int j = 0; j <= jmax; ++j) {
      // circle test run only once
      if ((i - c) * (i - c) + (j - c) * (j - c) <= c * c) {
        if (low < 0)
          low = j;
        high = j;
      }
    }
    row_low[i] = (low < 0) ? 0 : low;
    row_high[i] = high;
  }

  // ADDED: a cell gets grains only if a neighbor is inside the shape,
  // so pass 2 only needs columns low - 1 to high + 1 of this row and the rows above and below
  recv_low = (int*) malloc(N * sizeof(int));
  recv_high = (int*) malloc(N * sizeof(int));
  if (!recv_low || !recv_high)
    die("Malloc failed for receive bounds");
  for (int i = 0; i < N; ++i) {
    int low = N, high = -1; // empty by default
    for (int k = MAX(i - 1, 0); k <= MIN(i + 1, N - 1); ++k) {
      if (row_high[k] < row_low[k])
        continue; // empty row
      low = MIN(low, row_low[k] - 1);
      high = MAX(high, row_high[k] + 1);
    }
    recv_low[i] = MAX(low, 0);
    recv_high[i] = MIN(high, N - 1);
  }
}

// you can also use this function to debug print your sandpile
static void render(char *filename) {
  int chan = 3;
  unsigned char *pixels = (unsigned char *)malloc(N * N * chan * sizeof(char));
  int stride = N * chan;
  if (!pixels)
    die("malloc failed for PNG pixel buffer");
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      unsigned char r = 0, g = 0, b = 0;
      switch (grid[IDX(i, j)]) { // CHANGED: flat padded array idx
      case 0: // obsidian
        r = 15;
        g = 15;
        b = 25;
        break;
      case 1: // electric cyan
        r = 0;
        g = 210;
        b = 255;
        break;
      case 2: // vibrant magenta
        r = 230;
        g = 30;
        b = 130;
        break;
      case 3: // bright gold
        r = 255;
        g = 200;
        b = 0;
        break;
      default: // white
        r = 0;
        g = 0;
        b = 0;
      }

      int pid = (i * N + j) * chan;
      pixels[pid] = r;
      pixels[pid + 1] = g;
      pixels[pid + 2] = b;
    }
  }
  if (stbi_write_png(filename, N, N, chan, pixels, stride)) {
    printf("Successfully saved sandpile identity to: %s\n", filename);
  } else {
    die("Error: stb_image_write failed to save png file\n");
  }
  free(pixels);
}

static void stabilize() {
  // local copies so the compiler knows they can't change while we write to
  // the grid (before, re-read N and the row pointers from memory for
  // every cell because a write to grid[...] might have changed them)
  const int N_local = N;
  const int W = N + 2; // width of one padded row
  int* restrict g = grid; // g is the only pointer used to write the grid
  int* restrict tb = tbuf; // ADDED: only pointer used to write the topple counts
  const int* low = row_low;
  const int* high = row_high;
  const int* rlow = recv_low; // ADDED: local copy of recv bounds
  const int* rhigh = recv_high; // ADDED: local copy of recv bounds

  // ADDED: the Pacman is symmetric top to bottom (row i matches row N-1-i) when N is odd,
  // and every sweep keeps the grid symmetric, so only rows 0 to the middle row c need to be simulated
  // to keep scope, included a fallback for even N
  // where the middle falls between two rows and the circle isn't symmetric that way, so we do every row
  const int sym = (N_local % 2 == 1);
  const int c = N_local / 2; // middle row
  const int rows = sym ? c + 1 : N_local; // # of rows to simulate

  int changed = 0;
  // ADDED: start the threads once for the whole stabilize instead of once per pass
  // there are hundred thousands of sweeps, so starting threads twice per sweep would add up
  #pragma omp parallel
  {
    for (;;) { // CHANGED: every thread runs this loop
      #pragma omp single
      changed = 0; // CHANGED: one thread resets it and the rest wait at the end of single
      
      // pass 1 decides how many times each cell topples, using only its own value
      // so no cell depends on another cell's update in the same pass which lets gcc vectorize it
      // ADDED: rows are independent in this pass, so split them across threads
      // reduction(|:changed) gives each thread its own changed and ORs them together at the end
      // schedule(static, 16) hands out rows in chunks of 16 round robin, so every thread gets
      // a mix of short rows (top and bottom of the circle) and long rows (middle)
      #pragma omp for reduction(|:changed) schedule(static, 16)

      for (int i = 0; i < rows; ++i) { // CHANGED: use local copy, only top half + middle row
        int* restrict row = g + (i + 1) * W + 1; // ADDED: pointer to cell (i, 0) so row[j] is cell (i, j)
        int* restrict trow = tb + (i + 1) * W + 1; // ADDED: same for the topple counts
        const int jlow = low[i], jhigh = high[i]; // ADDED: read the row bounds once so gcc knows the trip count

        for (int j = jlow; j <= jhigh; ++j) { // CHANGED: use local copy, was j = 0 to MAX(i, N-1-i) + per-cell circle test
          // REMOVED: unpredictable branch

          int t = row[j] >> 2; // ADDED: # of topples
          changed |= t; // CHANGED: topple it
          row[j] -= 4 * t; // CHANGED: removes 4 grains per topple
          trow[j] = t; // ADDED: save t so pass 2 can hand it to neighbors
          // REMOVED: removed boundary checks since edge grains now land in the padding border
          // REMOVED: also the neighbor adds which are done in pass 2
        }
      }
      // ADDED: every thread copies the final changed now (all threads wait at the end of pass 1
      // so it's complete) and reading it after pass 2 instead could race with the next sweep resetting it
      const int any_toppled = changed;

      // ADDED: pass 2 gives every cell 1 grain per topple of each neighbor,
      // reading only tbuf and writing only grid, so no cell-to-cell dependency
      // covers shape cells and their neighbors, so cells outside the shape still collect grains like before
      // ADDED: each row only writes its own cells, so rows can also be split across threads here
      // the wait at the end of pass 1 makes sure all topple counts are saved before pass 2 reads them
      #pragma omp for schedule(static, 16)
      for (int i = 0; i < rows; ++i) { // CHANGED: only top half + middle row
        int* restrict row = g + (i + 1) * W + 1;
        const int* restrict trow = tb + (i + 1) * W + 1;
        const int* restrict tup = trow - W; // ADDED: topple counts of the row above
        const int* restrict tdown = (sym && i == c) ? tup : trow + W; // ADDED: topple counts of the row below
        // for the middle row that's row c+1, which isn't simulated, but it's the mirror of row c-1 so read that instead
        const int jlow = rlow[i], jhigh = rhigh[i];

        for (int j = jlow; j <= jhigh; ++j) {
          row[j] += tup[j] + tdown[j] + trow[j - 1] + trow[j + 1]; // CHANGED: using symmetry
        }
      }

      if (!any_toppled) // CHANGED: replaced while, every thread sees the same value so they all stop together
        break;
    }
  }
}

void find_identity() {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      grid[IDX(i, j)] = 6; // CHANGED: flat padded array idx
    }
  }
  stabilize();
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      grid[IDX(i, j)] = 6 - grid[IDX(i, j)]; // CHANGED: flat padded array idx
    }
  }
  stabilize();

  // ADDED: stabilize only filled in the top half and middle row when N is odd,
  // so copy each bottom row from its mirror row in the top half (whole padded row at once)
  if (N % 2 == 1) {
    for (int i = N / 2 + 1; i < N; ++i)
      memcpy(&grid[IDX(i, -1)], &grid[IDX(N - 1 - i, -1)], (size_t)(N + 2) * sizeof(int));
  }
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 2) {
    fprintf(stderr, "Usage: %s N\n", argv[0]);
    return EXIT_FAILURE;
  }
  char *end;
  uintmax_t parsed = strtoumax(argv[1], &end, 10);
  if (errno || argv[1][0] < '0' || argv[1][0] > '9' || *end || !parsed ||
      parsed > INT32_MAX)
    die("grid size must be a positive int");
  N = (int)parsed;

  // REMOVED: mallocs for grid and each row
  // ADDED: one alloc for the whole padded grid, calloc zeroes it so the border starts at 0
  grid = (int*) calloc((size_t) (N + 2) * (N + 2), sizeof(int));
  if (!grid)
    die("calloc failed for grid");
  // ADDED: topple counts, calloc zeroes them so cells outside the shape always give 0
  tbuf = (int*) calloc((size_t) (N + 2) * (N + 2), sizeof(int));
  if (!tbuf)
    die("calloc failed for topple buffer");

  compute_shape(); // ADDED: precompute shape bounds only once
  find_identity();

  render("identity.png");
  // REMOVED: loop freeing each row
  free(grid);
  free(tbuf); // ADDED: free topple counts
  free(row_low); // ADDED: free shape bounds
  free(row_high); // ADDED: free shape bounds
  free(recv_low); // ADDED: free recv bounds
  free(recv_high); // ADDED: free recv bounds
  return EXIT_SUCCESS;
}
