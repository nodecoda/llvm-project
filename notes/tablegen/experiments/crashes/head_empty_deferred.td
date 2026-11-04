// SIGABRT (134) in assertions build: Record.cpp:875
// One expression, no helper field: !filter cannot fold while N is a template
// arg; at final resolve it yields [] and !head asserts.
class C<int N> { int h = !head(!filter(x, [1, 2], !gt(x, N))); }
def D : C<5>;
