#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* getcwd (MinGW and POSIX both provide it). */

/* Pure path-string helpers (the Path namespace). Both '/' and '\' count as
   separators on input so Windows-style paths parse unchanged; output uses '/',
   which every File operation accepts on both platforms. */

static int is_sep(char c)
{
	return c == '/' || c == '\\';
}

/* A path is rooted if it starts with a separator or a drive prefix (c:). */
static int is_rooted(const char *p, int64_t n)
{
	if (n > 0 && is_sep(p[0]))
	{
		return 1;
	}

	if (n >= 2 && p[1] == ':')
	{
		return 1;
	}

	return 0;
}

/* Index just past the last separator, or 0 if there is none. */
static int64_t after_last_sep(const char *p, int64_t n)
{
	for (int64_t i = n; i > 0; i--)
	{
		if (is_sep(p[i - 1]))
		{
			return i;
		}
	}

	return 0;
}

void *bzy_path_join(void *a, void *b)
{
	const char *as = bzy_str_data(a);
	int64_t an = bzy_str_len(a);
	const char *bs = bzy_str_data(b);
	int64_t bn = bzy_str_len(b);

	if (an == 0 || is_rooted(bs, bn))
	{
		return bzy_str_new(bs, bn);   /* Second part wins. */
	}

	while (an > 0 && is_sep(as[an - 1]))   /* Drop a trailing separator on a. */
	{
		an--;
	}

	char *buf = malloc((size_t)an + 1 + (size_t)bn);
	memcpy(buf, as, (size_t)an);
	buf[an] = '/';
	memcpy(buf + an + 1, bs, (size_t)bn);
	void *out = bzy_str_new(buf, an + 1 + bn);
	free(buf);
	return out;
}

void *bzy_path_file_name(void *p)
{
	const char *s = bzy_str_data(p);
	int64_t n = bzy_str_len(p);
	int64_t at = after_last_sep(s, n);
	return bzy_str_new(s + at, n - at);
}

void *bzy_path_dir_name(void *p)
{
	const char *s = bzy_str_data(p);
	int64_t n = bzy_str_len(p);
	int64_t at = after_last_sep(s, n);
	if (at == 0)
	{
		return bzy_str_new("", 0);   /* No directory component. */
	}

	return bzy_str_new(s, at - 1);   /* Exclude the separator. */
}

void *bzy_path_extension(void *p)
{
	const char *s = bzy_str_data(p);
	int64_t n = bzy_str_len(p);
	int64_t base = after_last_sep(s, n);
	for (int64_t i = n; i > base; i--)
	{
		if (s[i - 1] == '.')
		{
			if (i - 1 == base)
			{
				break;               /* Leading dot (dotfile): no extension. */
			}

			return bzy_str_new(s + i - 1, n - (i - 1));
		}
	}

	return bzy_str_new("", 0);
}

/* Collapse '.' and '..' segments and duplicate separators in place. Operates on
   the part after any leading root, which is preserved verbatim. */
static int64_t normalize(char *buf, int64_t n)
{
	int64_t root = 0;
	if (n > 0 && buf[0] == '/')
	{
		root = 1;
	}
	else if (n >= 2 && buf[1] == ':')
	{
		root = (n >= 3 && buf[2] == '/') ? 3 : 2;
	}

	/* Walk segments of buf[root..n), writing kept ones back with single '/'. */
	int64_t w = root;
	int64_t i = root;
	while (i < n)
	{
		while (i < n && buf[i] == '/')   /* Skip separators. */
		{
			i++;
		}

		int64_t seg = i;
		while (i < n && buf[i] != '/')   /* Span one segment. */
		{
			i++;
		}

		int64_t len = i - seg;
		if (len == 0)
		{
			continue;
		}

		if (len == 1 && buf[seg] == '.')
		{
			continue;                    /* Drop '.'. */
		}

		if (len == 2 && buf[seg] == '.' && buf[seg + 1] == '.')
		{
			if (w > root)                /* Pop the previous kept segment. */
			{
				while (w > root && buf[w - 1] != '/')   /* Back over the segment. */
				{
					w--;
				}

				if (w > root)            /* Drop its preceding separator. */
				{
					w--;
				}

				continue;
			}
			/* Nothing to pop above the root: keep the '..'. */
		}

		if (w > root)
		{
			buf[w++] = '/';
		}

		memmove(buf + w, buf + seg, (size_t)len);
		w += len;
	}

	/* Strip a trailing '/' left above the root. */
	while (w > root && buf[w - 1] == '/')
	{
		w--;
	}

	return w;
}

void *bzy_path_absolute(void *p)
{
	const char *s = bzy_str_data(p);
	int64_t n = bzy_str_len(p);

	char *buf;
	int64_t bn;
	if (is_rooted(s, n))
	{
		buf = malloc((size_t)n + 1);
		memcpy(buf, s, (size_t)n);
		bn = n;
	}
	else
	{
		char cwd[4096];
		if (!getcwd(cwd, sizeof(cwd)))
		{
			cwd[0] = '.';
			cwd[1] = '\0';
		}

		int64_t cn = (int64_t)strlen(cwd);
		buf = malloc((size_t)cn + 1 + (size_t)n + 1);
		memcpy(buf, cwd, (size_t)cn);
		buf[cn] = '/';
		memcpy(buf + cn + 1, s, (size_t)n);
		bn = cn + 1 + n;
	}

	for (int64_t i = 0; i < bn; i++)   /* Unify separators before normalizing. */
	{
		if (buf[i] == '\\')
		{
			buf[i] = '/';
		}
	}

	bn = normalize(buf, bn);
	void *out = bzy_str_new(buf, bn);
	free(buf);
	return out;
}
