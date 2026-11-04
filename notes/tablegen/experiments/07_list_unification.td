//===- 07_list_unification.td - order-sensitive list typing -------------===//
// Two lexical traps + RecTy unification order:
//  * `[{` starts a *code block* (TGLexer::LexBracket), so a list whose first
//    element is a bit-list must be written `[ {0,1}, ...]` with a space.
//  * List literals unify element types left-to-right (resolveTypes), which
//    changes the internal element type / printed form for mixed int/bits.
def P { list<int> a = [1, {0,1}]; }      // int first: bits<2> -> int
def Q { list<int> b = [ {0,1}, 1]; }     // bits<2> first: int -> bits<2>
def S { list<bits<2>> c = [ {0,1}, {1,1}]; }
def T { list<bits<8>> d = [1, 2]; }
