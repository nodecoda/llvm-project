//===- 02_types.td - TableGen type system lab ----------------------------===//
class R { int n = 1; }
def RInst : R;
def RInst2 : R;
def add; def mul; def extra; def sub;

def T {
  // int / bit / bits
  int    i    = 42;
  bit    b    = 1;
  bits<8> b8  = 5;
  bits<4> hi  = b8{7...4};        // slice: MSB-first indexing
  bit     lsb = b8{0};
  bits<16> cat = {b8, b8};        // concat: first operand is most significant
  bits<8> fromInt = i;            // int -> bits
  int     fromBits = b8;          // bits -> int
  bits<4> small = 3;
  bits<8> widened = {small, small}; // bit concat is the only widening

  // strings
  string s1 = "abc";
  string s2 = s1 # 1 # b # "z";   // '#' paste accepts strings/ints/bits
  string s3 = "a" # "b";
  string s4 = !strconcat("x", "y", "z");
  string s5 = !substr("hello", 1, 3);
  int    s6 = !find("hello", "ll");
  bit    s7 = !match("hello", "h.*o");
  string s8 = !subst("a", "A", "banana");
  string s9 = !tolower("MiXeD");
  string s10 = !toupper("MiXeD");
  string s11 = !repr(b8);

  // lists
  list<int> empty = [];
  list<int> ints = [1, 2, 3];
  list<bits<8>> bl = [1, 2, 3];          // ints convert to bits<8>
  list<list<int>> ll = [[1], [2, 3]];
  list<int> flat = !listflatten(ll);
  list<int> doubled = !listconcat(ints, ints);
  list<int> splat = !listsplat(7, 3);
  list<int> removed = !listremove(ints, [2]);
  list<int> ranged = !range(1, 7, 2);
  list<int> sorted = !sort(x, [3, 1, 2], x);
  int head = !head(ints);
  list<int> tail = !tail(ints);
  int size = !size(ints);
  bit isEmpty = !empty(empty);

  // unset values and initialization state
  int maybe = ?;
  // NOTE: !initialized(<field that is ?>) does NOT fold; it errors. Only a
  // literal `?` or an initialized value folds (verified: expr_initialized).
  int initUnset = !initialized(?);
  int intsInit = !initialized(ints);

  // dag values
  dag d  = (add 1, "x", b8);
  dag d2 = !dag("mul", [2, 3], ["lhs", "rhs"]);
  string opname = !cast<string>(!getdagop(d));   // operator def name
  string rootName = !getdagopname((add:$root 1, 2)); // dag-root $name, NOT op name
  int arg0 = !getdagarg<int>(d, 0);
  string name1 = !getdagname(d, 1);
  dag d3 = !setdagarg(d, 0, 9);
  dag d4 = !setdagop(d, sub);
  dag d7 = !dag(!getdagop(d), [1, 2], ["a", "b"]);
  dag d5 = !setdagname(d, 1, "renamed");
  dag d6 = !con(d, (add 7));

  // record types and isa
  R rec = RInst;
  bit isR = !isa<R>(rec);
  list<R> recs = [RInst, RInst2];
  bit cmp = !eq(rec, RInst);

  // existence queries
  bit hasR = !exists<R>("RInst");
  bit hasQ = !exists<R>("Q");
}
