// Control flow: switch statements.  Since chapter 18 a dense switch becomes a
// jump table (BR_JT is custom, BRIND is legal), at -O0 as well as -O2, because
// the switch lowering runs in SelectionDAG; a sparse switch still becomes a
// comparison chain.  This file is compiled at every -O level by rvmin-e2e.sh,
// so it also guards the jump-table path against assembler/relocation drift.
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
