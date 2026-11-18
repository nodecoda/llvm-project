//===-- PyAst.h - CPython AST bridge for gpumin-triton ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 21: Triton's frontend starts from CPython's ast module, not from a
// hand-written Python parser.  This bridge runs the same parser and hands the
// result to the C++ frontend as JSON.  Everything after this file is C++.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_GPUMIN_TRITON_PYAST_H
#define LLVM_TOOLS_GPUMIN_TRITON_PYAST_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <string>

namespace gpumintriton {

/// A parsed Python module, already converted to JSON.
class PyAst {
public:
  /// Parse FilePath with the CPython interpreter named by Python.
  static llvm::Expected<PyAst> loadFromPython(llvm::StringRef FilePath,
                                              llvm::StringRef Python);
  /// Read an AST JSON file produced by --dump-ast-json.
  static llvm::Expected<PyAst> loadFromJson(llvm::StringRef JsonPath);
  /// Parse the JSON text directly.
  static llvm::Expected<PyAst> parseJson(llvm::StringRef Json,
                                         llvm::StringRef SourceName);

  const llvm::json::Object &root() const { return *Root.getAsObject(); }
  const llvm::json::Value &value() const { return Root; }
  llvm::StringRef sourcePath() const { return SourcePath; }

private:
  PyAst(llvm::json::Value Root, std::string SourcePath)
      : Root(std::move(Root)), SourcePath(std::move(SourcePath)) {}

  llvm::json::Value Root;
  std::string SourcePath;
};

} // namespace gpumintriton

#endif // LLVM_TOOLS_GPUMIN_TRITON_PYAST_H
