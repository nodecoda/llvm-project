//===- 05_let_and_instances.td - precedence + evaluation timing ----------===//
// Proves (a) let precedence, (b) implicit NAME, (c) template-arg defaults,
// (d) !instances/!exists are snapshots taken when each record is finalized.

class C { int x = 1; string s = "c"; }
let x = 2 in def D : C { let x = 3; }   // body wins: x = 3
let x = 2 in def E : C;                 // top-level let beats class default: x = 2
let s = "top" in def F : C;             // s = "top"

class NameUser { string n = NAME; }     // implicit template arg
def G : NameUser;                       // n = "G"

class Two<int a, int b = !add(a, 10), string n = "T"#a> {
  int va = a; int vb = b; string vn = n;
}
def H : Two<5>;                         // vb = 15, vn = "T5"
def I : Two<5, 7>;

multiclass MC<int a> { def _x : C { let x = !add(a, 100); } }
defm J : MC<1>;                         // J_x.x = 101

class R; def RA : R; def RB : R;
class L {
  list<R> seen = !instances<R>();       // snapshot at finalize time
  bit hasRB = !exists<R>("RB");
}
def LA : L;                             // sees RA, RB
def RC : R;
def LB : L;                             // sees RA, RB, RC
