// Control flow: branches, loops, switch, recursion.
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }

int gcd(int a, int b) {
  while (b) {
    int t = a % b;
    a = b;
    b = t;
  }
  return a;
}

int classify(int x) {
  switch (x) {
  case 0: return 10;
  case 1: return 11;
  case 2: return 12;
  case 3: return 13;
  case 4: return 14;
  default: return -1;
  }
}

int collatz(unsigned n) {
  int steps = 0;
  while (n != 1) {
    n = (n & 1) ? 3 * n + 1 : n / 2;
    steps++;
  }
  return steps;
}
