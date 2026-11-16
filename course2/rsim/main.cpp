//===-- main.cpp - rsim entry point, chapter 00 -----------------*- C++ -*-===//
//
// Course 2, chapter 00: just enough program to prove the build works.
// Later chapters add the ELF loader, the CPU and the debugger behind this
// same entry point.
//
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kVersion = "0.0";

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rsim %s (chapter 00)\n", kVersion);
    return 0;
  }

  std::fprintf(stderr, "usage: rsim --version\n"
                       "       (the loader and executor arrive in chapters "
                       "01-05)\n");
  return 2;
}
