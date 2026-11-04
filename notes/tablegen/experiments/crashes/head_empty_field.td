// SIGABRT (134) in assertions build: Record.cpp:875 "Empty list in head"
// Parse-time empty literals are rejected; a field that is empty only after
// resolution reaches UnOpInit::Fold with an empty ListInit.
def D { list<int> e = []; int h = !head(e); }
