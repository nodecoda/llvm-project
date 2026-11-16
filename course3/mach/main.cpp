//===-- main.cpp - rmach entry point, chapter 00 ----------------*- C++ -*-===//
//
// Course 3, chapter 00: just enough program to prove the build works.
// Later chapters add the bare-metal loader, the privileged machine, the MMU
// and the devices behind this same entry point.
//
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kVersion = "0.0";

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rmach %s (chapter 00)\n", kVersion);
    return 0;
  }

  std::fprintf(stderr, "usage: rmach --version\n"
                       "       (the loader and the machine arrive in "
                       "chapters 01-06)\n");
  return 2;
}
