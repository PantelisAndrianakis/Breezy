#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _tr = 0, _tp = 0;

#define RUN(name) do { \
	printf("  %-48s", #name); \
	name(); \
	printf("OK\n"); _tr++; _tp++; \
} while(0)

#define ASSERT(cond) do { \
	if (!(cond)) { \
		printf("FAIL\n    assertion: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
		_tr++; exit(1); \
	} \
} while(0)

#define ASSERT_INT(got, want) do { \
	long _g=(long)(got), _w=(long)(want); \
	if (_g != _w) { \
		printf("FAIL\n    expected %ld, got %ld  (%s:%d)\n", _w, _g, __FILE__, __LINE__); \
		_tr++; exit(1); \
	} \
} while(0)

#define ASSERT_STR(got, want) do { \
	if (strcmp((got),(want)) != 0) { \
		printf("FAIL\n    expected \"%s\", got \"%s\"  (%s:%d)\n", (want), (got), __FILE__, __LINE__); \
		_tr++; exit(1); \
	} \
} while(0)

#define SUMMARY() do { \
	printf("\n%d/%d passed\n", _tp, _tr); \
	if (_tp != _tr) exit(1); \
} while(0)
#endif
