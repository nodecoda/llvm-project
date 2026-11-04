// SIGSEGV (139): same path with an anonymous defm.
class C;
multiclass MC { dump "hi"; def D; }
defm : MC, C;
