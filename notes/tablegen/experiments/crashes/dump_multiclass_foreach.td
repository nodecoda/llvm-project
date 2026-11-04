// SIGSEGV (139): Dump nested in foreach inside the multiclass.
class C;
multiclass MC { foreach i = 0...1 in dump "hi"#i; def D; }
defm X : MC, C;
