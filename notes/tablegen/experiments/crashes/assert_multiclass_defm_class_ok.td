// Control: rc=0. Assertion entries are explicitly skipped by
// AddParentClass(RecordsEntry&) (TGParser.cpp:390-391).
class C;
multiclass MC { assert !eq(1, 1), "ok"; def D; }
defm X : MC, C;
