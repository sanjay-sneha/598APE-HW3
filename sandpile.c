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
int **grid;

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
      switch (grid[i][j]) {
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
  int changed = 0;
  do {
    changed = 0;
    for (int i = 0; i < N; ++i) {
      for (int j = row_low[i]; j <= row_high[i]; ++j) { // CHANGED: was j = 0 to MAX(i, N-1-i) + per-cell circle test
        // REMOVED: unpredictable branch

        int t = grid[i][j] >> 2; // ADDED: # of topples
        changed |= t; // CHANGED: topple it
        grid[i][j] -= 4 * t; // CHANGED: removes 4 grains per topple
        if (i > 0)
          grid[i - 1][j] += t; // CHANGED: adds 1 grain per topple to each neighbor
        if (j > 0)
          grid[i][j - 1] += t; // CHANGED: adds 1 grain per topple to each neighbor
        if (i < N - 1)
          grid[i + 1][j] += t; // CHANGED: adds 1 grain per topple to each neighbor
        if (j < N - 1)
          grid[i][j + 1] += t; // CHANGED: adds 1 grain per topple to each neighbor
      }
    }
  } while (changed);
}

void find_identity() {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      grid[i][j] = 6;
    }
  }
  stabilize();
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      grid[i][j] = 6 - grid[i][j];
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

  grid = (int **)malloc(N * sizeof(int *));
  for (int i = 0; i < N; ++i) {
    grid[i] = (int *)malloc(N * sizeof(int));
  }

  compute_shape(); // ADDED: precompute shape bounds only once
  find_identity();

  render("identity.png");
  for (int i = 0; i < N; ++i) {
    free(grid[i]);
  }
  free(grid);
  free(row_low); // ADDED: free shape bounds
  free(row_high); // ADDED: free shape bounds
  return EXIT_SUCCESS;
}
