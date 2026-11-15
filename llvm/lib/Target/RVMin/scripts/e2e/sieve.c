// Static storage, arrays and loops with no runtime support at all.
#define N 512
static unsigned char composite[N];

int sieve(int limit) {
  int count = 0;
  if (limit > N)
    limit = N;
  for (int i = 2; i * i < limit; i++) {
    if (composite[i])
      continue;
    for (int j = i * i; j < limit; j += i)
      composite[j] = 1;
  }
  for (int i = 2; i < limit; i++)
    if (!composite[i])
      count++;
  return count;
}

int collatz_max(unsigned n) {
  int best = 0;
  for (unsigned i = 1; i < n; i++) {
    unsigned x = i;
    int steps = 0;
    while (x != 1) {
      x = (x & 1) ? 3 * x + 1 : x >> 1;
      steps++;
    }
    if (steps > best)
      best = steps;
  }
  return best;
}
