//===- 01_core.td - TableGen object model lab ----------------------------===//
// Classes, defs, inheritance, let, multiclass/defm, foreach, defset,
// anonymous records, template args, name mangling.

class Base<int N, string S = "base"> {
  int n = N;
  string s = S;
  list<int> l = [N];
}

def D0 : Base<4>;
def D1 : Base<4, "one"> {
  let n = 5;              // def-level let wins over class default
}

class Derived : Base<1> {
  let s = "derived";      // subclass let wins over base default
}
def DD : Derived;

class Shadow  { int x = 1; string tag = "A"; }
class Shadow2 : Shadow { let x = 2; string tag = "B"; }
def DS  : Shadow2;
def DS2 : Shadow2 { let x = 3; }

//--- multiclass / defm ------------------------------------------------------
multiclass Leaf<int N, string S = "leaf"> {
  def _a : Base<N, S>;
  def _b : Base<!add(N, 1), S>;
}

defm M0 : Leaf<7>;
defm M1 : Leaf<7, "custom">;

let n = 42 in
defm M2 : Leaf<9>;        // let splits over every def produced by defm

multiclass Wrapper<int N> {
  defm _w : Leaf<!add(N, 100)>;   // nested defm inside a multiclass
  def _p : Base<N>;
}
defm W0 : Wrapper<1>;

//--- foreach ----------------------------------------------------------------
foreach i = 0...2 in
  def F#i : Base<i>;

foreach i = [10, 20] in {
  def L#i : Base<i>;
  def : Base<!add(i, 1)>;         // anonymous def inside foreach
}

foreach i = 0...1 in
  foreach j = 0...1 in
    def N#i#j : Base<!add(!mul(i, 10), j)>;

//--- defset -----------------------------------------------------------------
defset list<Base> MySet = {
  def S0 : Base<10>;
  def S1 : Base<11>;
}

def Sum {
  int total = !foldl(0, MySet, acc, r, !add(acc, r.n));
  int count = !size(MySet);
  string reprs = !interleave(!foreach(r, MySet, !repr(r)), " | ");
  list<string> names = !foreach(r, MySet, !cast<string>(r));
}

//--- defvar, top-level values ----------------------------------------------
defvar TopVar = 3;
def UsesVar { int x = TopVar; }

//--- anonymous defs ---------------------------------------------------------
def : Base<100>;
def : Base<101>;

//--- casts and record-typed values ----------------------------------------
def CastCheck {
  Base rec = D0;
  string recName = !cast<string>(rec);
  int viaName = !cast<Base>("D0").n;
  list<Base> insts = !instances<Base>();
}
