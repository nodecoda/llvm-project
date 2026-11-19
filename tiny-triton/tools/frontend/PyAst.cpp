//===-- PyAst.cpp - CPython AST bridge for tiny-triton ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PyAst.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

using namespace llvm;

namespace tinytriton {

namespace {

// Keep this in sync with the node fields the C++ frontend actually reads.
// The dumper is deliberately generic: unsupported nodes still reach the C++
// side so that Frontend can report them with a source location.
constexpr const char *AstDumper = R"PY(
import ast, json, sys

path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    source = f.read()

tree = ast.parse(source, filename=path)

def conv(node):
    if isinstance(node, ast.AST):
        out = {"_type": type(node).__name__}
        for field in node._fields:
            if hasattr(node, field):
                out[field] = conv(getattr(node, field))
        for attr in ("lineno", "col_offset", "end_lineno", "end_col_offset"):
            if hasattr(node, attr):
                out[attr] = getattr(node, attr)
        # JSON cannot distinguish 2 from 2.0 once llvm::json normalises an
        # integral double to int64, so the dumper records the Python type.
        if isinstance(node, ast.Constant) and isinstance(node.value, float):
            out["value_is_float"] = True
        return out
    if isinstance(node, list):
        return [conv(item) for item in node]
    if isinstance(node, (str, int, float, bool)) or node is None:
        return node
    return repr(node)

print(json.dumps(conv(tree), ensure_ascii=False))
)PY";

llvm::Expected<std::string> readFile(StringRef Path) {
  auto BufOrErr = MemoryBuffer::getFile(Path);
  if (!BufOrErr)
    return createStringError(BufOrErr.getError(), "cannot read '%s'",
                             Path.str().c_str());
  return (*BufOrErr)->getBuffer().str();
}

std::string readFileOrEmpty(StringRef Path) {
  auto OrErr = readFile(Path);
  if (!OrErr) {
    consumeError(OrErr.takeError());
    return {};
  }
  return std::move(*OrErr);
}

} // namespace

llvm::Expected<PyAst> PyAst::parseJson(StringRef Json, StringRef SourceName) {
  auto ValueOrErr = llvm::json::parse(Json);
  if (!ValueOrErr)
    return ValueOrErr.takeError();
  if (!ValueOrErr->getAsObject())
    return createStringError(inconvertibleErrorCode(),
                             "'%s' is not a JSON object", SourceName.str().c_str());
  return PyAst(std::move(*ValueOrErr), SourceName.str());
}

llvm::Expected<PyAst> PyAst::loadFromJson(StringRef JsonPath) {
  auto JsonOrErr = readFile(JsonPath);
  if (!JsonOrErr)
    return JsonOrErr.takeError();
  return parseJson(*JsonOrErr, JsonPath);
}

llvm::Expected<PyAst> PyAst::loadFromPython(StringRef FilePath,
                                            StringRef Python) {
  auto PythonOrErr = sys::findProgramByName(Python);
  if (!PythonOrErr)
    return createStringError(PythonOrErr.getError(),
                             "cannot find Python interpreter '%s'",
                             Python.str().c_str());

  SmallString<128> StdoutPath;
  SmallString<128> StderrPath;
  if (std::error_code EC = sys::fs::createTemporaryFile(
          "tiny-triton-ast", ".json", StdoutPath))
    return createStringError(EC, "cannot create temporary AST file");
  if (std::error_code EC = sys::fs::createTemporaryFile(
          "tiny-triton-ast", ".err", StderrPath)) {
    sys::fs::remove(StdoutPath);
    return createStringError(EC, "cannot create temporary error file");
  }

  SmallVector<StringRef, 5> Args;
  Args.push_back(*PythonOrErr);
  Args.push_back("-c");
  Args.push_back(AstDumper);
  Args.push_back(FilePath);

  std::optional<StringRef> Redirects[3] = {
      StringRef(), StringRef(StdoutPath), StringRef(StderrPath)};

  std::string ExecErr;
  bool ExecutionFailed = false;
  int RC = sys::ExecuteAndWait(*PythonOrErr, Args, std::nullopt, Redirects,
                               0, 0, &ExecErr, &ExecutionFailed);

  std::string StderrText = readFileOrEmpty(StderrPath);
  std::string StdoutText = readFileOrEmpty(StdoutPath);
  sys::fs::remove(StdoutPath);
  sys::fs::remove(StderrPath);

  if (ExecutionFailed || RC != 0) {
    std::string Message = "CPython ast.parse failed";
    if (!ExecErr.empty())
      Message += ": " + ExecErr;
    if (!StderrText.empty())
      Message += "\n" + StderrText;
    return createStringError(inconvertibleErrorCode(), "%s", Message.c_str());
  }
  if (StdoutText.empty())
    return createStringError(inconvertibleErrorCode(),
                             "CPython produced no AST JSON for '%s'",
                             FilePath.str().c_str());

  return parseJson(StdoutText, FilePath);
}

} // namespace tinytriton
