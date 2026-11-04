// SIGSEGV (139): Dump is the only entry in the multiclass body.
class C;
multiclass MC { dump "only"; }
defm X : MC, C;
