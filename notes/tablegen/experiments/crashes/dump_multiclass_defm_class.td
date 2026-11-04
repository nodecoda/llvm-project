// SIGSEGV (139): AddParentClass(RecordsEntry&) -> Entry.Loop->Entries
// (TGParser.cpp:393) on a Dump entry, called from ParseDefm (TGParser.cpp:4647).
class C;
multiclass MC { dump "hi"; def D; }
defm X : MC, C;
