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

// ADDED: per-row shape bound pointers
static int* row_low;
static int* row_high;

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
  const int* low = row_low;
  const int* high = row_high;

  int changed = 0;
  do {
    changed = 0;
    for (int i = 0; i < N_local; ++i) { // CHANGED: use local copy
      int* row = g + (i + 1) * W + 1; // ADDED: pointer to cell (i, 0) so row[j] is cell (i, j)
      for (int j = low[i]; j <= high[i]; ++j) { // CHANGED: use local copy, was j = 0 to MAX(i, N-1-i) + per-cell circle test
        // REMOVED: unpredictable branch

        int t = row[j] >> 2; // ADDED: # of topples
        changed |= t; // CHANGED: topple it
        row[j] -= 4 * t; // CHANGED: removes 4 grains per topple
        // REMOVED: removed boundary checks since edge grains now land in the padding border
        row[j - W] += t; // CHANGED
        row[j - 1] += t; // CHANGED
        row[j + W] += t; // CHANGED
        row[j + 1] += t; // CHANGED
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

  compute_shape(); // ADDED: precompute shape bounds only once
  find_identity();

  render("identity.png");
  // REMOVED: loop freeing each row
  free(grid);
  free(row_low); // ADDED: free shape bounds
  free(row_high); // ADDED: free shape bounds
  return EXIT_SUCCESS;
}
