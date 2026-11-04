def op;
class C { list<int> items = [2, 3]; string s = "b"; dag d = (op 1); }
def D : C {
  let append items = [4];       // list: !listconcat
  let prepend s = "a";          // string: !strconcat
  let append d = (op 2);        // dag: !con
}
