# Coding Style Guide

This document describes the C-specific coding conventions for this project.

---

## Core Principles

These are the core principles that define how we write code.

### 1. EXPLICIT TYPES - NO HIDDEN MEANING

C has no type inference, and that is a feature: every declaration names its type.
Keep it that way. Do not hide a type behind a macro or a `typedef` when the concrete
type is what the reader needs to see.

**Core Principle:** Code must be understandable without IDE assistance. The reader is more important than the writer.

```c
// CORRECT - explicit types are self-documenting.
int result = calculate_value(x, y, z);
uint8_t *data = get_data(&data_len);
size_t count = players.count;
```

**Typedef a struct for the name, not to hide the type:**
```c
// GOOD - the typedef names an aggregate; the reader still sees what it is.
typedef struct Buffer { uint8_t *bytes; size_t len; size_t cap; } Buffer;

// AVOID - a typedef that hides a pointer reads as a value but is not one.
typedef struct Node *NodeRef;   // The '*' is now invisible at the call site.
```

**Rule:** If understanding the type requires jumping to a definition, the declaration is doing too little work.

### 2. SINGLE-LINE CODE - NO WRAPPING

**Code must fit in the reader's working memory. If it does not fit on one line, it does not fit in the head either.**

Control flow, conditions, and signatures must stay on single lines. This enforces **locality of understanding**: all required information must be visible in one visual frame.

```c
// GOOD - all parameters visible, even if line is long.
void process_data(const char *source, const char *target, bool validate, int quality, ProcessingMode mode)
{
	// You can see everything. No hidden coupling. No indirection.
}

// WRONG - wrapping hides complexity.
void process_data(
	const char *source,
	const char *target,
	bool validate,
	int quality,
	ProcessingMode mode)
{
	// Now you have to scan vertically. Context is distributed.
}

// CORRECT - condition visible.
if (cursor[1] == '!' && cursor[2] == '[' && cursor[3] == 'C' && cursor[4] == 'D')
{
	handle_cdata();
}
```

**Why single-line?**
- **Visibility over abstraction** - You can see all parameters/conditions directly. No indirection. No hidden coupling.
- Your brain has ~7±2 working memory slots. Single-line keeps everything in one frame.
- Wrapping distributes complexity vertically - makes you scan and reconstruct context.
- Bundling parameters into a config struct to "fix" long lines makes things WORSE: hidden coupling, loss of transparency, harder debugging.
- A long single line is honest. It shows the real complexity. That's good.

**Don't wrap. Don't hide. If it's long, it's long. That's the truth.**

### 3. ALLMAN BRACES - ALWAYS ON NEW LINE
Opening braces `{` ALWAYS go on a new line. No exceptions.

```c
// WRONG.
if (condition) {
	do_something();
}

// CORRECT.
if (condition)
{
	do_something();
}
```

**Why?** Visual symmetry makes code easier to scan and spot errors.

### 4. TABS FOR INDENTATION - NOT SPACES
Use tabs, period. Configure your editor properly.

Why? Because a single tab character is the true, unambiguous representation of a single indentation level. Spaces are a visual approximation; tabs are the logical unit.

### 5. COMPLETE SENTENCES IN COMMENTS
Comments start with capital letter, end with period.

```c
// WRONG.
// calculate average value

// CORRECT.
// Calculate the average value.
```

**Why?** Professional code looks professional. We're not writing text messages.

---

## Naming Conventions

Get the names right or the code gets rejected.

### Variables, Functions, and Members
Use **snake_case**:

```c
int total_count = 0;
char file_name[256] = "data.txt";

void process_file(const char *path)
{
	int buffer_size = 1024;
}
```

**Module-prefix your exported functions.** A translation unit's public functions share
a short prefix naming the module, so a symbol's origin is obvious at the call site and
there are no link-time clashes - exactly as the runtime does (`bzy_pg_connect`,
`bzy_my_query`) and the compiler does (`resolve_func`, `cg_db_method`):

```c
int  bzy_pg_run_startup(void *sock, const char *user);   // The pgwire module.
void cg_db_method(Codegen *cg, Expr *e);                 // The codegen module.
```

### Structs, Enums, and Typedefs
Use **PascalCase**:

```c
typedef struct FileProcessor
{
	// Implementation.
} FileProcessor;

typedef struct ProcessingResult
{
	int original_size;
	int new_size;
} ProcessingResult;

typedef enum ProcessingMode
{
	PROCESSING_FAST,
	PROCESSING_BALANCED,
	PROCESSING_QUALITY
} ProcessingMode;
```

### Constants, Macros, and Enum Members
Use **SCREAMING_SNAKE_CASE**:

```c
#define MAX_BUFFER_SIZE 10000000
#define DEFAULT_TIMEOUT 30

static const char *APP_NAME = "Application";
```

---

## Formatting Rules

### Indentation
- **Tabs only** - no spaces for indentation.
- One tab per level.

### Braces Placement
Opening brace `{` on new line for:
- Functions
- Structs, enums, unions
- If/else blocks
- Loops (for, while, do-while)
- Switch statements

**Examples:**

```c
void process_file(const char *path)
{
	// Function body.
}

typedef struct FileProcessor
{
	// Struct body.
} FileProcessor;

if (condition)
{
	// If body.
}

for (int i = 0; i < count; i++)
{
	// Loop body.
}
```

### Spacing Rules

**Between functions - one blank line:**
```c
void function_one(void)
{
	// Implementation.
}

void function_two(void)
{
	// Implementation.
}
```

**Within functions - blank lines separate logic:**
```c
void process_data(const char *path)
{
	// Section 1: Read data.
	FileData data = read_file(path);
	size_t size = data.size;

	// Section 2: Process data.
	ProcessedData processed = transform(&data);
	OptimizedData optimized = optimize(&processed);

	// Section 3: Write results.
	write_output(&optimized);
}
```

**Between independent control structures - blank lines:**
```c
// CORRECT - blank lines separate independent checks.
if (p != NULL)
{
	key = p + 1;
}

if (params.dump_id_attributes)
{
	write_attribute(out, key, val);
}
```

**Related if/else stays together - no blank lines:**
```c
if (condition1)
{
	do_something();
}
else if (condition2)
{
	do_something_else();
}
else
{
	do_default();
}
```

**Critical spacing rules:**
- **Never more than one blank line** anywhere.
- **No trailing spaces** at end of lines.
- **No excessive spacing** like `if (x == 0)   `.

---

## Control Flow

### If-Else Statements
- **Always use braces** - even for single statements.
- **Keep conditions on single line** - no wrapping.
- **Include else only when both branches are meaningful** - early returns preferred for guard clauses.

```c
// Good - simple condition on single line.
if (cursor[1] == '!' && cursor[2] == '[' && cursor[3] == 'C' && cursor[4] == 'D')
{
	handle_cdata();
}

// Good - early return for guard clause (no else needed).
if (!is_valid(input))
{
	return -1;
}

process_valid_data(input);

// Good - else when both branches are meaningful.
if (file_exists(path))
{
	load_from_file(path);
}
else
{
	create_default_file(path);
}

// WRONG - no braces.
if (condition)
	do_something();
```

### Switch Statements
- **Always include a default case.**
- **Always include break** (unless fall-through is intentional and commented).
- **One statement per line** in case bodies - no crammed or column-aligned cases.
- **Only use braces when declaring variables** inside a case.

```c
switch (mode)
{
	case PROCESSING_FAST:
		apply_fast_processing();
		break;

	case PROCESSING_QUALITY:
	{
		// Braces only because we declare a variable.
		char *config = load_quality_config();
		apply_quality_processing(config);
		free(config);
		break;
	}

	default:
		fprintf(stderr, "Unknown mode.\n");
		break;
}
```

### Loops
Use the appropriate loop type:

```c
// For loops - known iteration count.
for (int i = 0; i < count; i++)
{
	process_item(i);
}

// For loops - iterating an array with its length.
for (size_t i = 0; i < file_count; i++)
{
	process_file(files[i]);
}

// While loops - condition-based iteration.
while (queue.count > 0)
{
	char *item = queue_pop(&queue);
	process_item(item);
}
```

---

## Type Declarations

### Explicit Types Always

C has no `auto`. Name every type. The benefit C++ spends effort recovering with
controlled inference, C gives for free.

```c
int result = calculate_value(x, y, z);
FileHandle file = open_file(path);
uint8_t *data = get_data(&len);
```

### Fixed-Width Integers
Use the `<stdint.h>` types when the width matters (wire formats, buffers, hashes):

```c
uint8_t  byte;
int32_t  count;
uint64_t hash;
size_t   length;   // For sizes and indices into memory.
```

### Const Correctness
Use `const` everywhere it applies - it documents intent and lets the compiler help.

```c
// Const pointer parameters - the function reads but does not modify.
void process(const char *text, const int *data, size_t data_len)
{
	// Use without modifying.
}

// Const variables.
const int max_retries = 3;
const char *config_file = "config.xml";
```

### Pointers
- C passes everything by value; use a pointer when you need to modify the caller's object, avoid a copy of a large struct, or express "may be absent" (NULL).
- Place `*` next to the variable, and give each declared pointer its own `*`.

```c
// Good - pointer for output and for large/owned data.
void process(const char *text);
uint8_t *create_buffer(size_t size);

// Each variable needs its own '*'.
int *ptr1;
int *ptr2;   // Not: int *ptr1, ptr2; (ptr2 would be int, not int*).

// NULL is the absent value; check it before dereferencing.
Node *found = find_node(list, key);
if (found != NULL)
{
	use(found);
}
```

---

## Comments

### General Rules
- **Start with a capital letter.**
- **End with a period.**
- **Use complete sentences.**
- **Use `//` for single-line** comments.
- **Use `/* */` for multi-line** comments.

```c
// Calculate the size reduction percentage.
double reduction_pct = (1.0 - ((double)new_size / original_size)) * 100.0;

/* This is a multi-line comment explaining
   a complex algorithm or process flow. */
```

### Documentation Comments
Describe a non-trivial public function in a block comment above it: what it does, what
the parameters mean, what it returns, and who owns any returned allocation.

```c
/* Process a file using the specified options. Returns 0 on success or a negative
   error code. On success *out_result is filled in; the caller owns nothing extra. */
int process_file(const char *source_path, const char *target_path, bool validate, ProcessingResult *out_result)
{
	// Implementation.
}
```

### Inline Comments
Only when clarifying non-obvious code, and never to state the obvious:

```c
// WRONG - obvious comment.
int x = 5; // Set x to 5.

// CORRECT - only comment when adding value.
int retry_count = 5; // Empirically determined optimal retry count.
```

---

## Headers and Includes

### Header Guards
Use `#pragma once` (or a classic `#ifndef` guard if a target compiler lacks it):

```c
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct MyType MyType;   // Opaque forward declaration.

MyType *my_type_create(size_t cap);
void    my_type_destroy(MyType *m);
```

### Include Ordering
Three groups, a blank line between each:

1. The corresponding header (for a `.c` file).
2. Standard library headers.
3. Project headers.

```c
#include "file_processor.h"   // Corresponding header.

#include <stdio.h>            // Standard library.
#include <stdlib.h>
#include <string.h>

#include "utils/helpers.h"    // Project headers.
#include "core/processor.h"
```

### Declarations in Headers, Definitions in .c
Headers declare the interface (function prototypes, struct layouts the caller needs,
constants). Put implementation in the `.c`. Keep a struct's layout out of the header
when callers should not depend on it - forward-declare it and expose create/destroy
plus accessor functions (the opaque-struct pattern).

---

## Structs and Modules

C has no classes. A "type with behaviour" is a `struct` plus a set of functions that
take a pointer to it as their first parameter, all sharing a module prefix.

### Struct Layout
Order members to minimise padding (largest alignment first) when it matters; otherwise
order by logical grouping.

```c
typedef struct XmlIndenter
{
	char  *xml_content;   // Pointers (8-byte) first.
	char  *indent_str;
	char  *eol_str;
	size_t content_len;
	bool   indent_only;   // Small members last.
} XmlIndenter;
```

### Create / Destroy Instead of Constructors and Destructors
A "constructor" is a `create` function that allocates and initialises; a "destructor"
is a `destroy` function that frees everything the type owns. They are paired: every
`create` has exactly one `destroy`.

```c
// Allocate + initialise. Returns NULL on allocation failure.
XmlIndenter *xml_indenter_create(const char *xml_content)
{
	XmlIndenter *x = calloc(1, sizeof(*x));
	if (x == NULL)
	{
		return NULL;
	}

	x->xml_content = strdup(xml_content);
	x->indent_str = strdup("\t");
	x->eol_str = strdup("\n");
	x->indent_only = true;
	return x;
}

// Free everything the instance owns, then the instance itself. NULL-safe.
void xml_indenter_destroy(XmlIndenter *x)
{
	if (x == NULL)
	{
		return;
	}

	free(x->xml_content);
	free(x->indent_str);
	free(x->eol_str);
	free(x);
}
```

### Methods Are Free Functions
A "method" is a function whose first parameter is the instance pointer.

```c
char *xml_indenter_run(XmlIndenter *x);                 // Mutating - non-const pointer.
const char *xml_indenter_get_eol(const XmlIndenter *x); // Read-only - const pointer.
```

### Stack Structs and Designated Initializers
For a plain value struct, prefer a stack instance with a designated initializer over a
heap allocation - no ownership to track.

```c
ProcessingResult result = { .original_size = size, .new_size = 0 };
```

---

## Error Handling

C has no exceptions. Report failure through return values and clean up explicitly.

### Return Codes
Return `0`/`true` on success and a negative error code (or `false`, or `NULL` for a
constructor) on failure. Hand richer results back through an out-parameter.

```c
int process_file(const char *path)
{
	if (!file_exists(path))
	{
		return -ENOENT;
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL)
	{
		return -EIO;
	}

	// Process file.
	fclose(f);
	return 0;
}
```

### Cleanup with a Single Exit (goto)
When a function acquires several resources, free them in reverse order at one exit
label rather than duplicating cleanup on every error path. This is the idiomatic C
substitute for RAII.

```c
int load_config(const char *path, Config *out_config)
{
	int rc = 0;
	FILE *f = NULL;
	char *buf = NULL;

	f = fopen(path, "rb");
	if (f == NULL)
	{
		rc = -EIO;
		goto done;
	}

	buf = malloc(MAX_CONFIG_SIZE);
	if (buf == NULL)
	{
		rc = -ENOMEM;
		goto done;
	}

	// ... parse into *out_config ...

done:
	free(buf);
	if (f != NULL)
	{
		fclose(f);
	}
	return rc;
}
```

### Caller-Side Checks
```c
ProcessingResult result;
int rc = process_file(path, &result);
if (rc != 0)
{
	fprintf(stderr, "Error: %s\n", strerror(-rc));
	return 1;
}

printf("Success: %d%% reduction.\n", result.reduction_percent);
```

---

## String Handling

C strings are NUL-terminated `char` arrays. There is no `std::string` - you manage the
memory and the length.

```c
const char *text = "Hello World";

// Length.
size_t len = strlen(text);

// Find a substring; strstr returns NULL when absent.
const char *pos = strstr(text, "World");
if (pos != NULL)
{
	// Found at (pos - text).
}
```

### Building Strings Safely
Always bound the write. Prefer `snprintf` over `sprintf`/`strcat`; it never overflows
and returns the length it wanted to write.

```c
char buf[64];
int n = snprintf(buf, sizeof(buf), "The answer is %d", value);
if (n < 0 || (size_t)n >= sizeof(buf))
{
	// Truncated or encoding error - handle it.
}
```

### Length-Counted Strings for Binary-Safe Data
When data may contain embedded NULs (network payloads, file contents), carry an
explicit length alongside the pointer rather than relying on `strlen`.

```c
typedef struct Slice { const char *data; size_t len; } Slice;
```

---

## Numeric Formatting

### Literals
Write plain numeric literals. (C23 allows the `'` digit separator; do not rely on it
unless the project targets C23.)

```c
#define MAX_BUFFER_SIZE 10000000
long very_large = 1234567890L;
double precise = 3.141592653;
```

### Hexadecimal and Binary
```c
uint32_t hex_value = 0xFFFFFF;   // Hex.
uint8_t  mask = 0xF0;            // Use hex for bit masks.
```
(Binary `0b` literals are a GCC/Clang extension and C23; avoid them in portable C.)

---

## Standard Library and Data Structures

C has no STL. Use plain arrays with an explicit length, and build the few data
structures you need or pull in a small, vetted library.

### Arrays + Length
The fundamental container is a pointer plus a count. Carry them together.

```c
int *numbers = malloc(count * sizeof(int));
// ... fill ...
for (size_t i = 0; i < count; i++)
{
	printf("%d\n", numbers[i]);
}
free(numbers);
```

### A Growable Array
When you need `push_back`, keep `count` and `cap` and grow geometrically.

```c
typedef struct IntVec { int *data; size_t count; size_t cap; } IntVec;

void int_vec_push(IntVec *v, int x)
{
	if (v->count == v->cap)
	{
		v->cap = v->cap ? v->cap * 2 : 8;
		v->data = realloc(v->data, v->cap * sizeof(int));
	}

	v->data[v->count++] = x;
}
```

### Sorting and Searching
Use the standard `qsort` and `bsearch` with a comparator rather than hand-rolling.

```c
qsort(numbers, count, sizeof(int), compare_int);
int key = 8;
int *found = bsearch(&key, numbers, count, sizeof(int), compare_int);
```

---

## Memory Management

Ownership is manual and must be obvious. Every allocation has exactly one owner
responsible for freeing it.

### Pair Every Allocation with a Free
- `malloc`/`calloc`/`realloc` is matched by exactly one `free`.
- `free(NULL)` is safe; lean on it to keep cleanup branch-free.
- After freeing a pointer that may be touched again, set it to `NULL`.

```c
uint8_t *buffer = malloc(size);
if (buffer == NULL)
{
	return -ENOMEM;   // Always check the result of an allocation.
}

// ... use buffer ...

free(buffer);
buffer = NULL;
```

### Document Ownership at the Boundary
A function that returns allocated memory must say so; the caller frees it. A function
that borrows a pointer must not free it.

```c
// Returns an owned string; the caller must free() it.
char *build_path(const char *dir, const char *name);

// Borrows `text`; does not free it.
size_t count_words(const char *text);
```

### Avoid Allocation in Hot Loops
Allocate once outside the loop and reuse the buffer.

```c
// WRONG - allocates every iteration.
for (size_t i = 0; i < file_count; i++)
{
	uint8_t *buffer = malloc(1024);
	process(files[i], buffer);
	free(buffer);
}

// CORRECT - allocate once.
uint8_t *buffer = malloc(1024);
for (size_t i = 0; i < file_count; i++)
{
	process(files[i], buffer);
}
free(buffer);
```

### Stack First
Prefer a fixed-size stack buffer when the size is small and bounded; reach for the heap
only when the size is large or dynamic. Do not use variable-length arrays for large or
untrusted sizes - they can overflow the stack silently.

---

## Data Layout in Performance-Sensitive Code

**Prefer contiguous memory in performance-sensitive code.** This is a hot-path
optimization, not a universal rule: where code is not performance-critical, choose the
layout that reads most clearly. Where the cache matters - tight loops, large data sets,
measured bottlenecks - the patterns below help.

### Prefer Contiguous Memory (in hot paths)
Use flat arrays. Avoid deep object graphs and pointer chasing.

```c
// CORRECT - contiguous arrays, cache-friendly.
typedef struct PlayerData
{
	Position *positions;
	int      *healths;
	uint8_t  *levels;
	size_t    count;
} PlayerData;

// WRONG - pointer-heavy, cache-hostile (a separate allocation per field per entity).
typedef struct Player
{
	Position *position;
	Item    **inventory;
	Stats    *stats;
} Player;
```

### Flat Data Structures
Avoid excessive indirection and nesting.

```c
// CORRECT - flat, direct access.
typedef struct GameState
{
	Player *players;  size_t player_count;
	Npc    *npcs;     size_t npc_count;
	Item   *items;    size_t item_count;
} GameState;

// WRONG - deep nesting: world -> zones -> entities, each a pointer hop.
```

### SoA vs AoS in Hot Paths
For a hot loop touching a single field, a Structure of Arrays beats an Array of
Structs - the loop streams one tightly-packed column instead of striding over whole
records.

```c
// AoS - good for general access.
typedef struct Player { float x; float y; int health; uint8_t level; } Player;

// SoA - better for a hot loop that only needs positions.
typedef struct Players { float *xs; float *ys; int *healths; uint8_t *levels; size_t count; } Players;

for (size_t i = 0; i < players.count; i++)
{
	update_position(players.xs[i], players.ys[i]);
}
```

These layouts pay off in hot loops; reach for them where profiling (or obvious scale) says the cache matters, not everywhere by default.

---

## Performance Considerations

### Pass Large Structs by Pointer
A `struct` passed by value is copied. For anything beyond a couple of words, pass a
`const` pointer.

```c
// Good - const pointer, no copy.
void process(const Config *config, const int *data, size_t data_len);

// Avoid - copies the whole struct on every call.
void process(Config config);
```

### Preallocate When the Size Is Known
```c
IntVec v = { 0 };
v.cap = 1000;
v.data = malloc(v.cap * sizeof(int));   // Avoid repeated realloc growth.
```

### Avoid Unnecessary Work in Loops
Hoist invariant computations and allocations out of the loop; reuse buffers (see Memory
Management).

---

## Anti-Patterns to Avoid

### ❌ Don't Ignore an Allocation Result
```c
// WRONG - dereferences a possibly-NULL pointer.
uint8_t *buf = malloc(size);
buf[0] = 0;

// CORRECT - check first.
uint8_t *buf = malloc(size);
if (buf == NULL)
{
	return -ENOMEM;
}
```

### ❌ Don't Declare Multiple Variables on the Same Line
```c
// WRONG - confusing, and the '*' binds to the variable, not the type.
int a, b = 0;
int *x, y;   // x is int*, y is int - almost never what you meant.

// CORRECT - one per line.
int a = 0;
int b = 0;
int *x = NULL;
int  y = 0;
```

### ❌ Don't Over-Engineer - Avoid Single-Use Abstractions
```c
// WRONG - helper called only once, no domain meaning.
void print_separator(void)
{
	printf("---\n");
}

// CORRECT - inline it.
printf("---\n");

// WRONG - pointless named constant with no domain meaning.
#define THREE 3
if (priority == THREE)

// CORRECT - use the value directly.
if (priority == 3)
```

**Exception:** Create abstractions when they are used multiple times, encode domain meaning (`#define MAX_PLAYERS 100`), improve clarity significantly, or are likely to change.

### ❌ Don't Leak on Error Paths
```c
// WRONG - returns without freeing buf.
char *buf = malloc(n);
if (parse(buf) != 0)
{
	return -1;   // Leak.
}

// CORRECT - free before returning (or use the goto-cleanup pattern).
char *buf = malloc(n);
int rc = parse(buf);
free(buf);
if (rc != 0)
{
	return -1;
}
```

### ❌ Don't Use Unbounded String Functions
```c
// WRONG - can overflow.
char dst[16];
strcpy(dst, src);
sprintf(dst, "%s-%d", name, id);

// CORRECT - bounded.
snprintf(dst, sizeof(dst), "%s-%d", name, id);
```

### ❌ Don't Hide Globals as Hidden State
Prefer passing state through parameters. A `static` file-scope variable is shared,
non-reentrant state - use it deliberately (and document the threading assumption), not
as a shortcut to avoid threading a parameter through.

### ❌ Don't Write Deeply Nested Code
```c
// WRONG - deeply nested.
if (condition1)
{
	if (condition2)
	{
		if (condition3)
		{
			// Too deep.
		}
	}
}

// CORRECT - early returns (guard clauses).
if (!condition1)
{
	return;
}

if (!condition2)
{
	return;
}

if (!condition3)
{
	return;
}

// Main logic at top level.
```

---

## Quick Reference Checklist

Before submitting code, verify:

- [ ] **Explicit types** - every declaration names its type; no type hidden behind a pointer typedef.
- [ ] **Fixed-width integers** - `<stdint.h>` types where width matters.
- [ ] **Contiguous memory in hot paths** - flat arrays where the cache matters; clearest layout elsewhere.
- [ ] **Single-line code** - all control flow, conditions, and signatures on single lines (no wrapping).
- [ ] **Allman braces** - opening `{` on a new line.
- [ ] **Tabs for indentation** - not spaces.
- [ ] **Complete sentences in comments** - capital letter, period.
- [ ] **snake_case** for variables/functions/members (module-prefixed public functions).
- [ ] **PascalCase** for structs/enums/typedefs.
- [ ] **SCREAMING_SNAKE_CASE** for constants/macros/enum members.
- [ ] **One blank line** between functions; never more than one anywhere.
- [ ] **No trailing spaces.**
- [ ] **One variable per line** - no `int a, b;`.
- [ ] **One statement per line** in switch cases.
- [ ] **Switch** - always include a `default`; one statement per case line.
- [ ] **Every malloc checked** and matched by exactly one `free`.
- [ ] **Ownership documented** at every function boundary that allocates or frees.
- [ ] **goto-cleanup** for multi-resource error paths; no leaks on any path.
- [ ] **Bounded string functions** - `snprintf`, never `strcpy`/`sprintf` into a fixed buffer.
- [ ] **const correctness** everywhere applicable.
- [ ] **#pragma once** for header guards; include order correct (corresponding, std, project).
- [ ] **Preallocate** arrays when the size is known; no allocation in hot loops.

---

## Summary

Remember these core principles:

1. **Explicit Everything** - Name every type; check every allocation; document every ownership transfer.
2. **Single-Line Code** - All control flow, conditions, and signatures on single lines. Wrapping hides complexity instead of reducing it.
3. **Allman Braces** - Opening braces on new lines, always.
4. **Complete Sentences** - Comments are documentation.
5. **Don't Over-Engineer** - YAGNI (You Ain't Gonna Need It).
6. **Manual Memory, Made Obvious** - One owner per allocation, paired create/destroy, goto-cleanup instead of RAII.
7. **Performance** - Pass large structs by pointer, preallocate, avoid allocations in loops, and prefer contiguous memory in hot paths.
