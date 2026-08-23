// DEBUG_LOG for the host tests.
//
// The real one expands to nothing under NDEBUG, which makes "if (x)
// DEBUG_LOG(...)" an empty statement and draws -Wempty-body. More usefully, a
// test can turn the output on when it is trying to work out why a case failed.

#ifndef TESTS_SHIM_DEBUG_H
#define TESTS_SHIM_DEBUG_H

#include <stdio.h>

namespace TestLog
{
	extern bool enabled;
}

#define DEBUG_LOG(...) \
	do { if (TestLog::enabled) fprintf(stderr, __VA_ARGS__); } while (0)

#endif
