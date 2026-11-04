// Control: rc=0. defvar does not become a RecordsEntry in NewEntries.
class C;
multiclass MC { defvar v = 1; def D; }
defm X : MC, C;
