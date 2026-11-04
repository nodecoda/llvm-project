//===- 03_ops.td - bang-operator semantics lab ---------------------------===//
class R { int n = 0; }
def RA : R { let n = 1; }
def RB : R { let n = 2; }
def add; def sub;

def P {
  //--- arithmetic (int is 64-bit, operations fold to IntInit) -------------
  int a1  = !add(1, 2, 3);
  int a2  = !sub(10, 3);
  int a3  = !mul(2, 3, 4);
  int a4  = !div(7, 2);       // truncation direction?
  int a5  = !div(-7, 2);
  int a6  = !not(0);
  int a7  = !not(5);
  int a8  = !and(6, 3);
  int a9  = !or(4, 1);
  int a10 = !xor(6, 3);
  int a11 = !shl(1, 4);
  int a12 = !srl(-8, 1);      // logical shift of a negative
  int a13 = !sra(-8, 1);      // arithmetic shift
  int a14 = !logtwo(8);
  int a15 = !mul(!add(2, 3), 4);

  //--- comparisons (also strings/bits/records) ---------------------------
  bit c1 = !eq(1, 1);
  bit c2 = !ne(1, 2);
  bit c3 = !lt(1, 2);
  bit c4 = !le(2, 2);
  bit c5 = !gt("b", "a");
  bit c6 = !eq(RA, RA);
  bit c7 = !eq(RA, RB);


  //--- conditionals ------------------------------------------------------
  int zero = 0;
  // !if is lazy during *resolution* (TernOpInit::resolveReferences
  // short-circuits), but every operator argument is still folded eagerly
  // while parsing, so a literal '!div(1, 0)' in a dead branch already fails.
  int l1 = !if(1, 7, !div(1, zero));      // cond is literal -> folds to 7
  int l2 = !if(zero, !div(1, zero), 8);   // cond resolves -> else only
  int cd = !cond(0: 1, 1: 2, true: 3);
  string sw = !switch("b", "a": "A", "b": "B", "DEFAULT");
  string swd = !switch("zz", "a": "A", "b": "B", "DEFAULT");

  //--- list operators ----------------------------------------------------
  list<int> li = [3, 1, 2, 1];
  list<int> sl = !sort(x, li, x);
  list<R> sl2 = !sort(x, [RA, RB], x.n);   // result keeps element type
  list<int> r1 = !range(4);
  list<int> r2 = !range(1, 4);
  list<int> r3 = !range(1, 7, 2);
  list<int> r4 = !range(0, 4, -1);   // empty
  list<int> r5 = !range(4, 0, -2);
  list<int> rm = !listremove(li, [1]);
  string il = !interleave(["a", "b", "c"], "-");
  int s1 = !size(li);
  list<int> noInts = [];
  bit e1 = !empty(noInts);
  int h1 = !head(li);
  list<int> t1 = !tail(li);
  list<int> f1 = !filter(x, li, !gt(x, 1));
  int fold = !foldl(0, li, acc, x, !add(acc, x));
  list<int> each = !foreach(x, li, !mul(x, 10));

  //--- string operators --------------------------------------------------
  int fnd = !find("banana", "na");
  int fnd2 = !find("banana", "zz");
  int fnd3 = !find("banana", "na", 3);
  bit m1 = !match("foo123", "[a-z]+[0-9]+");
  bit m2 = !match("foo", "[0-9]+");
  string sub = !substr("abcdef", 2);
  string sub2 = !substr("abcdef", 2, 2);
  string low = !tolower("AbC");
  string up = !toupper("AbC");
  string rep = !repr(RA);
  string repL = !repr([1, 2]);

  //--- casts and record queries -----------------------------------------
  // NOTE: !cast<int>("42") does NOT parse/fold (no string->int conversion);
  // string casts only work from records/ints to string, and record lookups.
  string fromInt = !cast<string>(42);
  bits<8> fromBitsCast = !cast<bits<8>>(5);
  R fromName = !cast<R>("RA");
  bit isA = !isa<R>(RA);
  bit ex = !exists<R>("RA");
  bit ex2 = !exists<R>("NOPE");
  list<R> insts = !instances<R>();
  list<R> instsRegex = !instances<R>("R[AB]");
}
