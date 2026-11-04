//===- 06_defset_nested.td - defset ordering/accumulation ---------------===//
// defset is an ordered list in declaration order, not SetTheory.
class A<int n> { int v = n; }

defset list<A> Outer = {
  def O0 : A<0>;
  defset list<A> Inner = {
    foreach i = 1...2 in
      def I#i : A<i>;
  }
  def O3 : A<3>;
}

def Q {
  list<string> outer = !foreach(a, Outer, !cast<string>(a));
  list<string> inner = !foreach(a, Inner, !cast<string>(a));
}
