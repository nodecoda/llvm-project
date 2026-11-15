// Memory: arrays, pointers, structs, strings, statics.
struct pair { int x; int y; };
struct mixed { char c; short s; int i; long long q; };

int garray[16];
static int sarray[8] = {1, 2, 3, 4, 5, 6, 7, 8};
const char *greeting = "hello rvmin";

int sum(int *a, int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += a[i];
  return t;
}

int fill_pair(struct pair *p, int x, int y) {
  p->x = x;
  p->y = y;
  return x + y;
}

int mixed_fields(struct mixed *m) {
  m->c = 1;
  m->s = 2;
  m->i = 3;
  m->q = 4;
  return m->c + m->s + m->i + (int)m->q;
}

int copy_struct(struct pair *dst, struct pair *src) {
  *dst = *src;
  return dst->x + dst->y;
}

int strlen_like(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

int use_globals(void) {
  garray[0] = sarray[0];
  garray[15] = sarray[7];
  return sum(garray, 16) + strlen_like(greeting);
}
