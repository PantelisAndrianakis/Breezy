#include "breezy.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* Immutable string layout (one allocation):
   0: vtable ptr  8: refcount  16: gcinfo  24: length  32: bytes[length+1]
   then an 8-byte cached-hash slot immediately after the NUL terminator.
   The data offset stays +32 (assumed across the runtime and the codegen FFI
   marshalling); the hash slot is appended so nothing else moves. It is zeroed at
   allocation (bzy_alloc zero-fills) and 0 means "not yet computed"; since strings
   are immutable, a once-computed content hash is valid forever. The map uses it to
   skip rehashing a reused key (see bzy_str_hashslot / map.c hash_key).
   The runtime owns the string's vtable/descriptor in C, since codegen never
   emits one. The descriptor has no finalizer and no object fields, so a string
   is reclaimed by a plain free and is invisible to the cycle collector. */

static int64_t g_string_typeinfo[2] = { 0 /* Finalizer. */, 0 /* Object-field count. */ };
static int64_t g_string_vtable[2];   /* [0] = typeinfo back-pointer (becomes vtable[-1]); [1] unused. */

static void *string_vtable(void)
{
	g_string_vtable[0] = (int64_t)&g_string_typeinfo[0];
	return &g_string_vtable[1];
}

void *bzy_str_new(const char *bytes, int64_t len)
{
	void *s = bzy_alloc(32 + len + 1 + 8);   /* +8: cached-hash slot after the NUL (zeroed by bzy_alloc). */
	*(void**)s = string_vtable();
	*(int64_t*)((char*)s + 24) = len;
	char *data = (char*)s + 32;
	if (len && bytes)
	{
		memcpy(data, bytes, (size_t)len);
	}

	data[len] = '\0';
	return s;
}

int64_t bzy_str_len(void *s)
{
	return *(int64_t*)((char*)s + 24);
}

const char *bzy_str_data(void *s)
{
	return (const char*)s + 32;
}

/* Address of the 8-byte cached content-hash slot (immediately after the NUL).
   Zeroed at allocation; 0 = not yet computed. Read/written via memcpy by the map,
   so no alignment is assumed. */
void *bzy_str_hashslot(void *s)
{
	return (char*)s + 32 + bzy_str_len(s) + 1;
}

void *bzy_str_concat(void *a, void *b)
{
	int64_t la = bzy_str_len(a), lb = bzy_str_len(b);
	void *c = bzy_str_new(NULL, la + lb);
	char *d = (char*)c + 32;
	memcpy(d, bzy_str_data(a), (size_t)la);
	memcpy(d + la, bzy_str_data(b), (size_t)lb);
	d[la + lb] = '\0';
	return c;
}

/* Copy a NUL-terminated C string into a fresh owned Breezy string. A NULL pointer
   yields the Breezy null reference (so an absent C value is distinguishable from
   an empty string). The copy is independent of the C buffer's lifetime. */
void *bzy_str_from_cstring(const char *p)
{
	if (!p)
	{
		return NULL;
	}

	return bzy_str_new(p, (int64_t)strlen(p));
}

/* Copy exactly len bytes (embedded NULs preserved) into a fresh owned Breezy
   string. A NULL pointer yields the Breezy null reference regardless of len. */
void *bzy_str_from_cbytes(const char *p, int64_t len)
{
	if (!p)
	{
		return NULL;
	}

	if (len < 0)
	{
		len = 0;
	}

	return bzy_str_new(p, len);
}

/* Copy a Breezy string's UTF-8 bytes into a fresh owned byte[] (stride-1 value
   array). Empty string -> empty array. The result does not alias the string. */
void *bzy_str_to_bytes(void *s)
{
	int64_t n = bzy_str_len(s);
	void *a = bzy_array_new_sized(n, 1, 0);   /* Unmanaged elements, zeroed. */
	if (n)
	{
		memcpy((char*)a + 32, (char*)s + 32, (size_t)n);
	}

	return a;
}

/* Build a fresh owned string from a byte[]/ubyte[]'s raw element bytes (stride 1).
   NULL array -> the Breezy null string (mirrors bzy_str_from_cbytes). */
void *bzy_str_from_bytes(void *arr)
{
	if (!arr)
	{
		return NULL;
	}

	return bzy_str_new((const char*)arr + 32, bzy_array_len(arr));
}

/* Join n strings into one owned (+1) result with a single allocation: sum the
   lengths, allocate once, then memcpy each piece. Replaces a chain of pairwise
   bzy_str_concat calls (O(n) allocations, O(n^2) copying) with O(n) copying. */
void *bzy_str_concat_n(void **parts, int64_t n)
{
	int64_t total = 0;
	for (int64_t i = 0; i < n; i++)
	{
		total += bzy_str_len(parts[i]);
	}

	void *c = bzy_str_new(NULL, total);            /* Allocates total + 1, already NUL-terminated. */
	char *d = (char*)c + 32;
	for (int64_t i = 0; i < n; i++)
	{
		int64_t li = bzy_str_len(parts[i]);
		if (li)
		{
			memcpy(d, bzy_str_data(parts[i]), (size_t)li);
		}

		d += li;
	}

	return c;
}

int64_t bzy_str_eq(void *a, void *b)
{
	if (a == b)
	{
		return 1;
	}

	if (!a || !b)
	{
		return 0;
	}

	int64_t la = bzy_str_len(a), lb = bzy_str_len(b);
	return (la == lb && memcmp(bzy_str_data(a), bzy_str_data(b), (size_t)la) == 0) ? 1 : 0;
}

/* A 32-bit FNV-1a content hash, NULL-safe. Used by synthesized record hashCode
   for string fields so value-equal strings always hash the same. */
int64_t bzy_str_hashcode(void *s)
{
	if (!s)
	{
		return 0;
	}

	int64_t n = bzy_str_len(s);
	const unsigned char *p = (const unsigned char*)bzy_str_data(s);
	uint32_t h = 2166136261u;
	for (int64_t i = 0; i < n; i++)
	{
		h = (h ^ p[i]) * 16777619u;
	}

	return (int64_t)h;
}

/* First index of needle in s, or -1. An empty needle matches at 0. */
static int64_t str_find(const char *s, int64_t sl, const char *n, int64_t nl)
{
	if (nl == 0)
	{
		return 0;
	}

	if (nl > sl)
	{
		return -1;
	}

	for (int64_t i = 0; i + nl <= sl; i++)
	{
		if (memcmp(s + i, n, (size_t)nl) == 0)
		{
			return i;
		}
	}

	return -1;
}

int64_t bzy_str_index_of(void *s, void *needle)
{
	return str_find(bzy_str_data(s), bzy_str_len(s), bzy_str_data(needle), bzy_str_len(needle));
}

int64_t bzy_str_contains(void *s, void *needle)
{
	return bzy_str_index_of(s, needle) >= 0 ? 1 : 0;
}

int64_t bzy_str_starts_with(void *s, void *pre)
{
	int64_t sl = bzy_str_len(s), pl = bzy_str_len(pre);
	return (pl <= sl && memcmp(bzy_str_data(s), bzy_str_data(pre), (size_t)pl) == 0) ? 1 : 0;
}

int64_t bzy_str_ends_with(void *s, void *suf)
{
	int64_t sl = bzy_str_len(s), fl = bzy_str_len(suf);
	return (fl <= sl && memcmp(bzy_str_data(s) + sl - fl, bzy_str_data(suf), (size_t)fl) == 0) ? 1 : 0;
}

void *bzy_str_substring(void *s, int64_t start, int64_t end)
{
	int64_t sl = bzy_str_len(s);
	if (start < 0)
	{
		start = 0;
	}

	if (end > sl)
	{
		end = sl;
	}

	if (start > end)
	{
		start = end;
	}

	if (start == 0 && end == sl)
	{
		bzy_retain(s);   /* Full-range slice: reuse the immutable original. */
		return s;
	}

	return bzy_str_new(bzy_str_data(s) + start, end - start);
}

void *bzy_str_replace(void *s, void *from, void *to)
{
	const char *t = bzy_str_data(s);
	int64_t tl = bzy_str_len(s);
	const char *f = bzy_str_data(from);
	int64_t fl = bzy_str_len(from);
	const char *r = bzy_str_data(to);
	int64_t rl = bzy_str_len(to);
	if (fl == 0)
	{
		return bzy_str_new(t, tl);                 /* Empty needle: no-op (avoid looping). */
	}

	int64_t cap = tl + 16, n = 0;
	char *buf = malloc((size_t)cap);
	int64_t pos = 0;
	while (pos <= tl - fl)
	{
		if (memcmp(t + pos, f, (size_t)fl) == 0)
		{
			if (n + rl > cap)
			{
				while (n + rl > cap)
				{
					cap *= 2;
				}

				buf = realloc(buf, (size_t)cap);
			}

			memcpy(buf + n, r, (size_t)rl);
			n += rl;
			pos += fl;
		}
		else
		{
			if (n + 1 > cap)
			{
				cap *= 2;
				buf = realloc(buf, (size_t)cap);
			}

			buf[n++] = t[pos++];
		}
	}

	int64_t tail = tl - pos;
	if (n + tail > cap)
	{
		cap = n + tail;
		buf = realloc(buf, (size_t)cap);
	}

	memcpy(buf + n, t + pos, (size_t)tail);        /* Trailing bytes after the last match. */
	n += tail;
	void *out = bzy_str_new(buf, n);
	free(buf);
	return out;
}

static int is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

void *bzy_str_trim(void *s)
{
	const char *t = bzy_str_data(s);
	int64_t sl = bzy_str_len(s), a = 0, b = sl;
	while (a < b && is_ws(t[a]))
	{
		a++;
	}

	while (b > a && is_ws(t[b - 1]))
	{
		b--;
	}

	if (a == 0 && b == sl)
	{
		bzy_retain(s);   /* Nothing to trim: reuse the original. */
		return s;
	}

	return bzy_str_new(t + a, b - a);
}

void *bzy_str_to_upper(void *s)
{
	int64_t sl = bzy_str_len(s);
	const char *t = bzy_str_data(s);
	int64_t i = 0;
	while (i < sl && !(t[i] >= 'a' && t[i] <= 'z'))
	{
		i++;
	}

	if (i == sl)
	{
		bzy_retain(s);   /* Already upper-case: no copy needed. */
		return s;
	}

	char *buf = malloc((size_t)sl + 1);
	for (int64_t j = 0; j < sl; j++)
	{
		char c = t[j];
		buf[j] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}

	void *o = bzy_str_new(buf, sl);
	free(buf);
	return o;
}

void *bzy_str_to_lower(void *s)
{
	int64_t sl = bzy_str_len(s);
	const char *t = bzy_str_data(s);
	int64_t i = 0;
	while (i < sl && !(t[i] >= 'A' && t[i] <= 'Z'))
	{
		i++;
	}

	if (i == sl)
	{
		bzy_retain(s);   /* Already lower-case: no copy needed. */
		return s;
	}

	char *buf = malloc((size_t)sl + 1);
	for (int64_t j = 0; j < sl; j++)
	{
		char c = t[j];
		buf[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}

	void *o = bzy_str_new(buf, sl);
	free(buf);
	return o;
}

int64_t bzy_str_equals_ignore_case(void *a, void *b)
{
	int64_t la = bzy_str_len(a), lb = bzy_str_len(b);
	if (la != lb)
	{
		return 0;
	}

	const char *pa = bzy_str_data(a), *pb = bzy_str_data(b);
	for (int64_t i = 0; i < la; i++)
	{
		char ca = pa[i], cb = pb[i];
		if (ca >= 'A' && ca <= 'Z')
		{
			ca = (char)(ca + 32);
		}

		if (cb >= 'A' && cb <= 'Z')
		{
			cb = (char)(cb + 32);
		}

		if (ca != cb)
		{
			return 0;
		}
	}

	return 1;
}

int64_t bzy_str_is_empty(void *s)
{
	return bzy_str_len(s) == 0 ? 1 : 0;
}

/* 1 if the string is non-empty and every byte is a digit 0-9. Empty -> 0. */
int64_t bzy_str_is_numeric(void *s)
{
	int64_t n = bzy_str_len(s);
	if (n == 0)
	{
		return 0;
	}

	const char *d = bzy_str_data(s);
	for (int64_t i = 0; i < n; i++)
	{
		if (d[i] < '0' || d[i] > '9')
		{
			return 0;
		}
	}

	return 1;
}

/* 1 if the string is non-empty and every byte is a letter or digit. Empty -> 0. */
int64_t bzy_str_is_alphanumeric(void *s)
{
	int64_t n = bzy_str_len(s);
	if (n == 0)
	{
		return 0;
	}

	const char *d = bzy_str_data(s);
	for (int64_t i = 0; i < n; i++)
	{
		char c = d[i];
		int is_digit = (c >= '0' && c <= '9');
		int is_lower = (c >= 'a' && c <= 'z');
		int is_upper = (c >= 'A' && c <= 'Z');
		if (!is_digit && !is_lower && !is_upper)
		{
			return 0;
		}
	}

	return 1;
}

int64_t bzy_str_char_at(void *s, int64_t i)
{
	int64_t sl = bzy_str_len(s);
	if (i < 0 || i >= sl)
	{
		return -1;
	}

	return (unsigned char)bzy_str_data(s)[i];
}

int64_t bzy_str_last_index_of(void *s, void *needle)
{
	int64_t sl = bzy_str_len(s), nl = bzy_str_len(needle);
	const char *d = bzy_str_data(s), *n = bzy_str_data(needle);
	if (nl == 0)
	{
		return sl;
	}

	if (nl > sl)
	{
		return -1;
	}

	for (int64_t i = sl - nl; i >= 0; i--)
	{
		if (memcmp(d + i, n, (size_t)nl) == 0)
		{
			return i;
		}
	}

	return -1;
}

void *bzy_str_repeat(void *s, int64_t n)
{
	int64_t sl = bzy_str_len(s);
	if (n <= 0 || sl == 0)
	{
		return bzy_str_new("", 0);
	}

	int64_t total = sl * n;
	char *buf = malloc((size_t)total);
	const char *d = bzy_str_data(s);
	for (int64_t k = 0; k < n; k++)
	{
		memcpy(buf + k * sl, d, (size_t)sl);
	}

	void *o = bzy_str_new(buf, total);
	free(buf);
	return o;
}

/* Split core. When is_set, each byte of `delim` is its own separator (split on
   any one of them); otherwise `delim` is a single substring separator. With
   skip_empty, empty fields are dropped. An empty delimiter yields the whole
   string as one field (subject to skip_empty). */
static void *split_core(void *s, void *delim, int is_set, int64_t skip_empty)
{
	const char *t = bzy_str_data(s);
	int64_t tl = bzy_str_len(s);
	const char *d = bzy_str_data(delim);
	int64_t dl = bzy_str_len(delim);

	void **items = NULL;
	int64_t count = 0, cap = 0;

#define SPLIT_PUSH(p, n)                                                       \
	do {                                                                       \
		if (!(skip_empty && (n) == 0)) {                                       \
			if (count == cap) {                                                \
				cap = cap ? cap * 2 : 8;                                       \
				items = realloc(items, (size_t)cap * sizeof(void *));          \
			}                                                                  \
			items[count++] = bzy_str_new((p), (n));                            \
		}                                                                      \
	} while (0)

	if (dl == 0)
	{
		SPLIT_PUSH(t, tl);
	}
	else
	{
		int64_t start = 0, i = 0;
		while (i < tl)
		{
			int hit = 0;
			int64_t step = 0;
			if (is_set)
			{
				if (memchr(d, t[i], (size_t)dl))
				{
					hit = 1;
					step = 1;
				}
			}
			else if (i + dl <= tl && memcmp(t + i, d, (size_t)dl) == 0)
			{
				hit = 1;
				step = dl;
			}

			if (hit)
			{
				SPLIT_PUSH(t + start, i - start);
				i += step;
				start = i;
			}
			else
			{
				i++;
			}
		}

		SPLIT_PUSH(t + start, tl - start);   /* Final field. */
	}

#undef SPLIT_PUSH

	void *arr = bzy_array_new(count, 1);             /* Managed string elements. */
	void **elems = (void**)((char*)arr + 32);
	for (int64_t k = 0; k < count; k++)
	{
		elems[k] = items[k];
	}

	free(items);
	return arr;
}

void *bzy_str_split(void *s, void *sep)
{
	return split_core(s, sep, 0, 0);
}

void *bzy_str_split_opt(void *s, void *sep, int64_t skip_empty)
{
	return split_core(s, sep, 0, skip_empty);
}

void *bzy_str_split_any(void *s, void *chars)
{
	return split_core(s, chars, 1, 0);
}

void *bzy_str_split_any_opt(void *s, void *chars, int64_t skip_empty)
{
	return split_core(s, chars, 1, skip_empty);
}

void bzy_print_str(void *s)
{
	fwrite(bzy_str_data(s), 1, (size_t)bzy_str_len(s), stdout);
	fputc('\n', stdout);
}

/* StringBuilder layout (one object; owns a separate doubling buffer):
   0: vtable  8: refcount  16: gcinfo  24: length  32: capacity  40: buf ptr.
   The descriptor carries a finalizer that frees buf before the object is freed;
   it has no object fields (buf is raw bytes), so the cycle collector ignores it. */

static int64_t *SB_LEN(void *sb)
{
	return (int64_t*)((char*)sb + 24);
}

static int64_t *SB_CAP(void *sb)
{
	return (int64_t*)((char*)sb + 32);
}

static char **SB_BUF(void *sb)
{
	return (char**)((char*)sb + 40);
}

static void bzy_sb_finalize(void *sb)
{
	free(*SB_BUF(sb));
}

static int64_t g_sb_typeinfo[2] = { 0 /* Finalizer (set on first use). */, 0 /* Object-field count. */ };
static int64_t g_sb_vtable[2];   /* [0] = typeinfo back-pointer (becomes vtable[-1]); [1] unused. */

static void *sb_vtable(void)
{
	g_sb_typeinfo[0] = (int64_t)(void*)bzy_sb_finalize;
	g_sb_vtable[0] = (int64_t)&g_sb_typeinfo[0];
	return &g_sb_vtable[1];
}

void *bzy_sb_new(void)
{
	void *sb = bzy_alloc(48);   /* Length/capacity/buf are zeroed by bzy_alloc. */
	*(void**)sb = sb_vtable();
	return sb;
}

void bzy_sb_append_cstr(void *sb, const char *bytes, int64_t len)
{
	int64_t used = *SB_LEN(sb), cap = *SB_CAP(sb);
	if (used + len > cap)
	{
		int64_t ncap = cap ? cap : 16;
		while (ncap < used + len)
		{
			ncap *= 2;
		}

		char *nb = realloc(*SB_BUF(sb), (size_t)ncap);
		if (!nb)
		{
			abort();
		}

		*SB_BUF(sb) = nb;
		*SB_CAP(sb) = ncap;
	}

	if (len)
	{
		memcpy(*SB_BUF(sb) + used, bytes, (size_t)len);
	}

	*SB_LEN(sb) = used + len;
}

void bzy_sb_append(void *sb, void *s)
{
	bzy_sb_append_cstr(sb, bzy_str_data(s), bzy_str_len(s));
}

void *bzy_sb_to_string(void *sb)
{
	return bzy_str_new(*SB_BUF(sb), *SB_LEN(sb));
}

/* String -> number parsing. A parse op sets g_parse_error on malformed input; the
   codegen-emitted bzy_number_check then throws NumberFormatException, locating the
   call site with pc/frame (the bzy_io_check / bzy_oob pattern). */

extern char __vtable_NumberFormatException[];   /* Emitted per-program by codegen. */

static __thread const char *g_parse_error;       /* Set by a parse op; consumed by bzy_number_check. */

static void parse_fail(const char *msg)
{
	g_parse_error = msg;                          /* Static strings only (stable lifetime). */
}

void bzy_number_check(int64_t pc, int64_t frame)
{
	if (!g_parse_error)
	{
		return;
	}

	void *msg = bzy_str_new(g_parse_error, (int64_t)strlen(g_parse_error));
	g_parse_error = NULL;
	void *exc = bzy_alloc(32);
	*(void**)exc = (void*)__vtable_NumberFormatException;
	*(void**)((char*)exc + 24) = msg;             /* Exception.message. */
	bzy_throw(exc, pc, frame);                     /* Never returns. */
}

/* Parse the whole string as a base-10 long. *ok=0 on failure (empty / trailing
   garbage / overflow). Leading whitespace is skipped by strtoll. */
static long long parse_ll(void *s, int *ok)
{
	const char *d = bzy_str_data(s);
	char *end;
	errno = 0;
	long long v = strtoll(d, &end, 10);
	*ok = (end != d && *end == '\0' && errno == 0);
	return v;
}

int64_t bzy_str_to_long(void *s)
{
	int ok;
	long long v = parse_ll(s, &ok);
	if (!ok)
	{
		parse_fail("String.toLong: not a long integer.");
		return 0;
	}

	return (int64_t)v;
}

int64_t bzy_str_to_int(void *s)
{
	int ok;
	long long v = parse_ll(s, &ok);
	if (!ok || v < -2147483647LL - 1 || v > 2147483647LL)
	{
		parse_fail("String.toInt: not a 32-bit integer.");
		return 0;
	}

	return (int64_t)v;
}

int64_t bzy_str_to_short(void *s)
{
	int ok;
	long long v = parse_ll(s, &ok);
	if (!ok || v < -32768 || v > 32767)
	{
		parse_fail("String.toShort: not a 16-bit integer.");
		return 0;
	}

	return (int64_t)v;
}

int64_t bzy_str_to_byte(void *s)
{
	int ok;
	long long v = parse_ll(s, &ok);
	if (!ok || v < -128 || v > 127)
	{
		parse_fail("String.toByte: not an 8-bit integer.");
		return 0;
	}

	return (int64_t)v;
}

double bzy_str_to_double(void *s)
{
	const char *d = bzy_str_data(s);
	char *end;
	double v = strtod(d, &end);
	if (end == d || *end != '\0')
	{
		parse_fail("String.toDouble: not a number.");
		return 0;
	}

	return v;
}

float bzy_str_to_float(void *s)
{
	const char *d = bzy_str_data(s);
	char *end;
	float v = strtof(d, &end);
	if (end == d || *end != '\0')
	{
		parse_fail("String.toFloat: not a number.");
		return 0;
	}

	return v;
}

static int str_ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if (ca != cb)
		{
			return 0;
		}
	}

	return *a == '\0' && *b == '\0';
}

int64_t bzy_str_to_bool(void *s)
{
	const char *d = bzy_str_data(s);
	if (str_ieq(d, "true"))
	{
		return 1;
	}

	if (str_ieq(d, "false"))
	{
		return 0;
	}

	parse_fail("String.toBool: not a bool.");
	return 0;
}
