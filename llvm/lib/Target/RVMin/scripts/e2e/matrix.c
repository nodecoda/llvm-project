// Memory: arrays, pointer arithmetic, nested loops and unsigned division.
// Everything here goes through the load/store patterns of chapter 6 and the
// stack addressing of chapter 6/7; no library calls are needed except the
// division the target does not have.
#define N 6

int mat[N][N];

void fill(int seed) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++)
      mat[i][j] = (i * N + j) * seed;
}

int trace(void) {
  int t = 0;
  for (int i = 0; i < N; i++)
    t += mat[i][i];
  return t;
}

int sum_row(int r) {
  int s = 0;
  const int *p = mat[r];
  for (int i = 0; i < N; i++)
    s += p[i];
  return s;
}

unsigned mean_abs(void) {
  unsigned total = 0;
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      int v = mat[i][j];
      total += v < 0 ? (unsigned)-v : (unsigned)v;
    }
  return total / (N * N);
}

int dot(int r, int c) {
  int s = 0;
  for (int i = 0; i < N; i++)
    s += mat[r][i] * mat[i][c];
  return s;
}
