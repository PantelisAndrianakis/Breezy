#include "prelude.h"

/* Built-in vector types, written in Breezy and compiled into every program.
   They are ordinary classes: escape analysis stack-allocates non-escaping
   locals (no heap/ARC), ARC handles escapes, and calculateDistance is just
   Math.sqrt. Distance precision matches the element type (float for *f, else
   double); equals is exact component-wise comparison. */

static const char VEC2I[] =
	"class Vector2i\n"
	"{\n"
	"	int x;\n"
	"	int y;\n"
	"	Vector2i(int x, int y) { this.x = x; this.y = y; }\n"
	"	boolean equals(Vector2i o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector2i o)\n"
	"	{\n"
	"		int dx = this.x - o.x;\n"
	"		int dy = this.y - o.y;\n"
	"		return Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC2L[] =
	"class Vector2l\n"
	"{\n"
	"	long x;\n"
	"	long y;\n"
	"	Vector2l(long x, long y) { this.x = x; this.y = y; }\n"
	"	boolean equals(Vector2l o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector2l o)\n"
	"	{\n"
	"		long dx = this.x - o.x;\n"
	"		long dy = this.y - o.y;\n"
	"		return Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC2F[] =
	"class Vector2f\n"
	"{\n"
	"	float x;\n"
	"	float y;\n"
	"	Vector2f(float x, float y) { this.x = x; this.y = y; }\n"
	"	boolean equals(Vector2f o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	float calculateDistance(Vector2f o)\n"
	"	{\n"
	"		float dx = this.x - o.x;\n"
	"		float dy = this.y - o.y;\n"
	"		return (float)Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC2D[] =
	"class Vector2d\n"
	"{\n"
	"	double x;\n"
	"	double y;\n"
	"	Vector2d(double x, double y) { this.x = x; this.y = y; }\n"
	"	boolean equals(Vector2d o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector2d o)\n"
	"	{\n"
	"		double dx = this.x - o.x;\n"
	"		double dy = this.y - o.y;\n"
	"		return Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

const char *BZY_PRELUDE[] = { VEC2I, VEC2L, VEC2F, VEC2D };
const int   BZY_PRELUDE_COUNT = 4;
