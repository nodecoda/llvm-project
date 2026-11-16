//===-- selftest.h - hand-computed instruction vectors, chapter 03 -*- C++ -*-===//

#ifndef RSIM_SELFTEST_H
#define RSIM_SELFTEST_H

namespace rsim {

/// Run every instruction-level unit test; returns the number of failures.
int runSelfTest();

} // namespace rsim

#endif // RSIM_SELFTEST_H
