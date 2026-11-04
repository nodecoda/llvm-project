// Control: rc=0. Without a trailing regular class parent, ParseDefm never
// calls AddParentClass(RecordsEntry&); the dump note is printed normally.
multiclass MC { dump "hi"; def D; }
defm X : MC;
