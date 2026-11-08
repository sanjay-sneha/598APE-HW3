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

  int changed = 0;
  do {
    changed = 0;
    // pass 1 decides how many times each cell topples, using only its own value
    // so no cell depends on another cell's update in the same pass which lets gcc vectorize it
    for (int i = 0; i < N_local; ++i) { // CHANGED: use local copy
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

    // ADDED: pass 2 gives every cell 1 grain per topple of each neighbor,
    // reading only tbuf and writing only grid, so no cell-to-cell dependency
    // covers shape cells and their neighbors, so cells outside the shape still collect grains like before
    for (int i = 0; i < N_local; ++i) {
      int* restrict row = g + (i + 1) * W + 1;
      const int* restrict trow = tb + (i + 1) * W + 1;
      const int jlow = rlow[i], jhigh = rhigh[i];
      for (int j = jlow; j <= jhigh; ++j) {
        row[j] += trow[j - W] + trow[j + W] + trow[j - 1] + trow[j + 1];
      }
    }
  } while (changed);
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
