//===-- switch.c - dense/sparse switches under QEMU, chapter 18 --*- C -*-===//
//
// Compiled with clang at -O2 (which is where the jump table is built),
// lowered by `llc -march=rvmin`, linked against rvmin-rt.S + start.S, and
// executed under QEMU user mode.
//
// main() returns 0 when every check passes, otherwise the number of the first
// failing check, which becomes the process exit code.
//
//===----------------------------------------------------------------------===//

int dense(int x) {
  switch (x) {
  case 0: return 100;
  case 1: return 101;
  case 2: return 102;
  case 3: return 103;
  case 4: return 104;
  case 5: return 105;
  case 6: return 106;
  case 7: return 107;
  default: return -1;
  }
}

int sparse(int x) {
  switch (x) {
  case -1000: return 1;
  case 7: return 2;
  case 42: return 3;
  case 999: return 4;
  default: return 0;
  }
}

int fallthrough(int x) {
  int r = 0;
  switch (x) {
  case 0: r += 1; /* fall through */
  case 1: r += 2; /* fall through */
  case 2: r += 4; break;
  case 3: r += 8; break;
  default: r = -1;
  }
  return r;
}

int nested(int a, int b) {
  switch (a & 3) {
  case 0: return b + 1;
  case 1:
    switch (b & 1) {
    case 0: return 10;
    default: return 11;
    }
  case 2: return b - 1;
  default: return b * 2;
  }
}

// volatile keeps the selector unknown at compile time, so the switch really
// has to branch at run time instead of folding to a constant.
volatile int selector = 3;
volatile int selector_b = 2;

int main(void) {
  for (int i = 0; i < 8; i++) {          // dense: table hit
    selector = i;
    if (dense(selector) != 100 + i)
      return 1;
  }
  selector = 8;                          // dense: default
  if (dense(selector) != -1)
    return 2;

  selector = 42;                         // sparse: comparison chain
  if (sparse(selector) != 3)
    return 3;
  selector = 41;
  if (sparse(selector) != 0)
    return 4;

  selector = 0;                          // fallthrough chain
  if (fallthrough(selector) != 7)
    return 5;
  selector = 2;
  if (fallthrough(selector) != 4)
    return 6;

  selector = 1;                          // nested switch
  selector_b = 0;
  if (nested(selector, selector_b) != 10)
    return 7;
  selector_b = 1;
  if (nested(selector, selector_b) != 11)
    return 8;
  selector = 3;
  if (nested(selector, 5) != 10)
    return 9;

  return 0;
}
