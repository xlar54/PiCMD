#include "harness.h"

namespace Harness
{
	int checks = 0;
	int failures = 0;
	const char* currentCase = "(none)";

	void BeginCase(const char* name)
	{
		currentCase = name;
	}

	void Fail(const char* file, int line, const char* expr, const char* detail)
	{
		++failures;
		fprintf(stderr, "FAIL  %s\n        %s:%d\n        %s\n",
			currentCase, file, line, expr);
		if (detail)
			fprintf(stderr, "        %s\n", detail);
	}

	int Summary()
	{
		if (failures)
			fprintf(stderr, "\n%d of %d checks failed\n", failures, checks);
		else
			fprintf(stderr, "all %d checks passed\n", checks);
		return failures ? 1 : 0;
	}
}
