//===- LegalizerCoreEval.h - Evaluate legalizer rules at build time -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A rule set answers as a chain: the first rule whose predicate holds decides.
// The Core IR spells each predicate as an AST, which is data, so the same
// answer can be computed here, at generation time, without a compiled
// subtarget.
//
// What that buys: a cell stops being "whatever the chain says at run time" and
// becomes a value the generator can write down, compare against a frozen
// answer, and enumerate over every feature assignment.  What it costs: this is
// a second implementation of the predicates, and the two can disagree.  Every
// leaf therefore mirrors its counterpart in LegalityPredicates and the
// LegalizeRuleSet builders one at a time, and anything that cannot be
// evaluated is reported, never guessed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_UTILS_TABLEGEN_LEGALIZERCOREEVAL_H
#define LLVM_UTILS_TABLEGEN_LEGALIZERCOREEVAL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGenTypes/LowLevelType.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace llvm {
namespace legeval {

/// One argument of a predicate leaf.  The Core IR spells a leaf as
/// {"kind":"leaf","name":...,"args":[...]}, and an argument is an integer, a
/// string, or a list of strings (typeIn).
struct Arg {
  enum Kind { Int, Str, StrList } K = Int;
  int64_t I = 0;
  std::string S;
  std::vector<std::string> L;
};

/// A predicate node: a leaf, a negation, or an n-ary all_of / any_of.
///
/// The tree arrives as text because the Core IR is how it is written down.
/// Parsing it here is the seam to delete once the nodes are built directly;
/// until then it keeps the evaluator off the emitter's internals.
struct Node {
  enum Kind { Leaf, Not, All, Any } K = Leaf;
  std::string Name; // leaf name, or "all_of" / "any_of"
  std::vector<Arg> Args;
  std::vector<Node> Kids;
};

/// Parse the serialized predicate that starts at \p I.  On failure \p Err says
/// what was expected, and the node must not be treated as evaluable.
bool parseNode(StringRef S, size_t &I, Node &N, std::string &Err);

/// theta: the feature booleans the rules are written in.
///
/// A condition in the Core IR is written in the target's words -- either the
/// record name ("HasBWI") or the condition text it stands for
/// ("Subtarget.hasBWI()") -- and a compound condition such as
/// "UseX87 && !HasSSE1" arrives as text with the declared conditions spliced
/// in.  Both spellings are answered here.  A call that is not one of the
/// declared conditions is reported: its value is not part of theta, and
/// guessing it would make the table a fiction.
class Theta {
public:
  void add(StringRef Name, StringRef Condition, bool Value);
  bool evalName(StringRef Name, bool &Ok) const;
  /// The value of a condition, in either spelling.
  bool eval(StringRef Text, bool &Ok) const;

private:
  StringMap<bool> ByName;
  std::vector<std::pair<std::string, bool>> ByCondition; // longest first
};

/// The query a rule is evaluated against: one type per slot, an optional
/// memory descriptor, and theta.
///
/// Types are real LLTs, not (kind, element, lanes) triples, so size and
/// element questions are answered by the same code the chain uses.
struct Env {
  ArrayRef<LLT> Types;
  bool HasMem = false;
  LLT MemTy;
  uint64_t MemAlignInBits = 0;
  /// A type spelling to the LLT it names.  A spelling with no entry is a gap in
  /// the description, not a type that is merely unequal to every other.
  std::function<const LLT *(StringRef)> NamedType;
  /// A feature condition, as the Core IR carries it, to its value under theta.
  std::function<bool(StringRef, bool &)> Feature;
  /// Where nogoods go.  Non-empty means "this was not evaluated", which is not
  /// the same answer as "this did not match".
  std::string *Err = nullptr;
};

/// One Core IR rule, in the order the chain applies them.
struct Rule {
  std::string Action;
  unsigned TypeIdx = 0;
  Node Pred;
};

/// What a rule set answers for one point.
struct Verdict {
  bool Matched = false; // false: no rule held, which is not "Unsupported"
  std::string Action;
  unsigned TypeIdx = 0;
  std::string Why; // why the answer could not be computed, if it could not
};

/// The value of a bound such as "16" or "HasBWI ? 32 : (HasAVX2 ? 16 : 8)".
/// Returns false when a leaf of the expression has no value under theta.
bool evalBound(const std::string &Text, const Env &E, uint64_t &Out);

/// The first rule whose predicate holds.  \p Why is empty only when every
/// predicate on the way was evaluable.
Verdict apply(ArrayRef<Rule> Rules, const Env &E);

} // namespace legeval
} // namespace llvm

#endif