// Crashes llvm-tblgen: AddParentClass(RecordsEntry&) dereferences Entry.Loop
// for a Dump entry (TGParser.cpp:385-401).
class C;
multiclass MC { dump "hi"; def D; }
defm X : MC, C;
