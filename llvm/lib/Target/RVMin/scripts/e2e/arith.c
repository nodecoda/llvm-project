// Integer arithmetic: everything chapter 4, 9 and 10 taught, in one file.
int gmul, gdiv, grem;

int mul(int a, int b) { return a * b; }
int divide(int a, int b) { return a / b; }
unsigned udivide(unsigned a, unsigned b) { return a / b; }
int mod(int a, int b) { return a % b; }
unsigned umod(unsigned a, unsigned b) { return a % b; }

int shifts(int x, int n) {
  int a = x << n;
  int b = x >> n;                 // arithmetic
  unsigned c = (unsigned)x >> n;  // logical
  return a ^ b ^ (int)c;
}

int bits(unsigned x) {
  return __builtin_popcount(x) + __builtin_clz(x | 1) + __builtin_ctz(x | 8);
}

long long wide(long long a, long long b) {
  long long m = a * b;
  return (m >> 3) + (a / (b | 1)) + (b % (a | 1));
}

int div_pow2(int x) { return x / 16 + x % 32; }
int mul_pow2(int x) { return x * 64; }

void globals(int a, int b) {
  gmul = a * b;
  gdiv = a / b;
  grem = a % b;
}
