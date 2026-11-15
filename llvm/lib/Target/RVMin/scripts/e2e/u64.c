// Integer arithmetic: everything chapter 9 taught about i64.  RV32I has no
// 64-bit registers, so every operation here is a pair of i32 with carry
// propagation, and the variable-count shifts become __ashldi3/__lshrdi3
// calls (the compiler runtime provides them in chapter 12).
long long add64(long long a, long long b) { return a + b; }
long long sub64(long long a, long long b) { return a - b; }
long long mul64(long long a, long long b) { return a * b; }
long long div64(long long a, long long b) { return a / (b | 1); }
unsigned long long udiv64(unsigned long long a, unsigned long long b) {
  return a / (b | 1);
}
long long shl64(long long a, int n) { return a << (n & 63); }
long long sar64(long long a, int n) { return a >> (n & 63); }
unsigned long long shr64(unsigned long long a, int n) {
  return a >> (n & 63);
}
int cmp64(long long a, long long b) {
  if (a < b) return -1;
  if (a > b) return 1;
  return (int)(a ^ b);
}
int trunc64(long long a) { return (int)a; }
long long ext64(int a, unsigned b) { return (long long)a + (long long)b; }
