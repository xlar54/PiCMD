// A test harness small enough not to be a dependency.
//
// No framework: this project cross compiles for bare metal and pulling gtest
// in for the host build would be more machinery than the tests are worth. A
// case is a function, CHECK reports and keeps going so one failure does not
// hide the next, and main returns non-zero if anything failed - which is all
// CI needs.

#ifndef TESTS_HARNESS_H
#define TESTS_HARNESS_H

#include <stdio.h>
#include <string.h>

namespace Harness
{
	extern int checks;
	extern int failures;
	extern const char* currentCase;

	void BeginCase(const char* name);
	void Fail(const char* file, int line, const char* expr, const char* detail);
	int  Summary();
}

#define CHECK(expr) \
	do { \
		++Harness::checks; \
		if (!(expr)) Harness::Fail(__FILE__, __LINE__, #expr, 0); \
	} while (0)

#define CHECK_MSG(expr, msg) \
	do { \
		++Harness::checks; \
		if (!(expr)) Harness::Fail(__FILE__, __LINE__, #expr, msg); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		++Harness::checks; \
		long long va = (long long)(a), vb = (long long)(b); \
		if (va != vb) { \
			char buf[160]; \
			snprintf(buf, sizeof(buf), "got %lld, expected %lld", va, vb); \
			Harness::Fail(__FILE__, __LINE__, #a " == " #b, buf); \
		} \
	} while (0)

#define TEST_CASE(name) Harness::BeginCase(name)

#endif
