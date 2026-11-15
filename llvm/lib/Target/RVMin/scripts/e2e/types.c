// Narrow and 64-bit types: chapter 9.
signed char gsc;
unsigned char guc;
short gss;
unsigned short gus;

int narrow(signed char a, unsigned char b, short c, unsigned short d) {
  gsc = a; guc = b; gss = c; gus = d;
  return gsc + guc + gss + gus;
}

int narrow_loads(signed char *p, unsigned short *q) {
  return p[1] + q[2] + p[3] + q[4];
}

long long wide_add(long long a, long long b) { return a + b; }
long long wide_cmp(long long a, long long b) { return a < b ? a : b; }

int bools(_Bool a, _Bool b) {
  _Bool c = a && b;
  return c + (a || b) + (a != b);
}

long long array64(long long *p, int i) {
  long long t[4];
  t[i & 3] = p[i];
  return t[0] + t[1] + t[2] + t[3];
}
