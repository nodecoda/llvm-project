//===- StudyEmitter.cpp - Introspection backend for learning TableGen ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A small, read-only introspection backend used by the TableGen study notes.
// It demonstrates the whole RecordKeeper/Record API surface that real
// backends use: getClasses/getDefs/getGlobals, direct superclasses, template
// arguments, RecordVal types and anonymous record naming.
//
// Run with:  llvm-tblgen -gen-study input.td
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {

class StudyEmitter {
  const RecordKeeper &Records;

public:
  explicit StudyEmitter(const RecordKeeper &RK) : Records(RK) {}

  void run(raw_ostream &OS) {
    emitSourceFileHeader("TableGen study report", OS, Records);

    ArrayRef<const Record *> Classes = Records.getClasses();
    ArrayRef<const Record *> Defs = Records.getDefs();

    // Direct-derived counts are computed in one pass over classes + defs.
    StringMap<unsigned> DirectDerived;
    unsigned Anonymous = 0;
    unsigned DefValues = 0;
    for (const Record *D : Defs) {
      Anonymous += D->isAnonymous();
      DefValues += D->getValues().size();
      for (const auto &SC : D->getDirectSuperClasses())
        ++DirectDerived[SC.first->getName()];
    }
    for (const Record *C : Classes)
      for (const auto &SC : C->getDirectSuperClasses())
        ++DirectDerived[SC.first->getName()];

    OS << "input: " << Records.getInputFilename() << "\n";
    OS << "classes: " << Classes.size() << "  defs: " << Defs.size()
       << "  anonymous defs: " << Anonymous
       << "  globals: " << Records.getGlobals().size()
       << "  values on defs (incl. inherited): " << DefValues << "\n";

    unsigned RootClasses = 0, ClassTA = 0;
    for (const Record *C : Classes) {
      RootClasses += C->getDirectSuperClasses().empty();
      ClassTA += !C->getTemplateArgs().empty();
    }
    OS << "root classes: " << RootClasses
       << "  classes with template args: " << ClassTA << "\n\n";

    // Field type histogram over defs.
    StringMap<unsigned> TypeHist;
    for (const Record *D : Defs)
      for (const RecordVal &RV : D->getValues())
        ++TypeHist[RV.getType()->getAsString()];
    std::vector<std::pair<unsigned, std::string>> Types;
    for (const auto &KV : TypeHist)
      Types.emplace_back(KV.second, KV.first().str());
    llvm::sort(Types, [](const auto &A, const auto &B) {
      if (A.first != B.first)
        return A.first > B.first;
      return A.second < B.second;
    });
    OS << "== field types (defs) ==\n";
    unsigned Shown = 0;
    for (const auto &T : Types) {
      if (Shown++ == 12)
        break;
      OS << "  " << T.first << "\t" << T.second << "\n";
    }

    // Top classes by direct subclass count.
    std::vector<std::pair<unsigned, StringRef>> Top;
    for (const auto &KV : DirectDerived)
      Top.emplace_back(KV.second, KV.first());
    llvm::sort(Top, [](const auto &A, const auto &B) {
      if (A.first != B.first)
        return A.first > B.first;
      return A.second < B.second;
    });
    OS << "\n== classes by direct derived records ==\n";
    for (unsigned I = 0, E = std::min<unsigned>(Top.size(), 10); I != E; ++I)
      OS << "  " << Top[I].first << "\t" << Top[I].second << "\n";

    // Show the first classes with their template args and parents.
    OS << "\n== first 10 classes ==\n";
    for (unsigned I = 0, E = std::min<unsigned>(Classes.size(), 10); I != E;
         ++I) {
      const Record *C = Classes[I];
      OS << "  class " << C->getName() << " (values="
         << C->getValues().size() << ", targs=" << C->getTemplateArgs().size()
         << ", parents=" << C->getDirectSuperClasses().size() << ")\n";
    }

    // Anonymous defs are real defs named anonymous_N at final resolution.
    OS << "\n== first 10 anonymous defs ==\n";
    unsigned Printed = 0;
    for (const Record *D : Defs) {
      if (!D->isAnonymous())
        continue;
      OS << "  " << D->getName() << "\n";
      if (++Printed == 10)
        break;
    }
  }
};

} // end anonymous namespace

static TableGen::Emitter::OptClass<StudyEmitter>
    X("gen-study", "Print a TableGen study/introspection report");
