// RUN: llvm-tblgen -print-records %s | FileCheck %s

// Check that string values are escaped when printed as TableGen source, so
// that a printed record can be parsed back to the same values. Code strings
// are printed verbatim.

class C {
  string Plain = "plain";
  string Quote = "a\"b";
  string Backslash = "a\\b";
  string Tab = "a\tb";
  string Newline = "a\nb";
  code Raw = [{raw "quotes" and \backslash}];
}

def D : C;

// CHECK: string Plain = "plain";
// CHECK: string Quote = "a\"b";
// CHECK: string Backslash = "a\\b";
// CHECK: string Tab = "a\tb";
// CHECK: string Newline = "a\nb";
// CHECK: code Raw = [{raw "quotes" and \backslash}];
