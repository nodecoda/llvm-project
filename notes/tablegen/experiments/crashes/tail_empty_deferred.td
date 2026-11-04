// SIGABRT (134) in assertions build: Record.cpp:882 "Empty list in tail"
class C<int N> { list<int> t = !tail(!filter(x, [1, 2], !gt(x, N))); }
def D : C<5>;
