#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

int bzy_ir_enabled(void)
{
	static int cached = -1;
	if (cached < 0)
	{
		/* Default ON: eligible call-free functions use the IR backend (the hot-spill
		   safety gate keeps any function the IR would lose on the emitter). Set
		   BZY_IR=0 to force the emitter everywhere. */
		const char *v = getenv("BZY_IR");
		cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
	}

	return cached;
}

/* Append each double-quoted token in `val` to the string table at `base`
   (each slot `width` bytes, `cap` slots), bumping *count. Tokens longer than
   the slot are truncated. */
static void parse_string_array(const char *val, char *base, int width, int cap, int *count)
{
	const char *p = val;
	while (*p)
	{
		if (*p == '"')
		{
			const char *start = ++p;
			while (*p && *p != '"')
			{
				p++;
			}

			int len = (int)(p - start);
			if (*count < cap)
			{
				char *slot = base + (*count) * width;
				if (len >= width)
				{
					len = width - 1;
				}

				memcpy(slot, start, len);
				slot[len] = '\0';
				(*count)++;
			}

			if (*p == '"')
			{
				p++;
			}
		}
		else
		{
			p++;
		}
	}
}

void config_parse_links(const char *text, LinkConfig *cfg)
{
	int in_link = 0;
	const char *p = text;
	while (*p)
	{
		char line[1024];
		int n = 0;
		while (*p && *p != '\n' && n < (int)sizeof(line) - 1)
		{
			line[n++] = *p++;
		}

		line[n] = '\0';
		if (*p == '\n')
		{
			p++;
		}

		/* Trim leading whitespace. */
		char *s = line;
		while (*s == ' ' || *s == '\t' || *s == '\r')
		{
			s++;
		}

		if (*s == '#' || *s == '\0')
		{
			continue;   /* Comment or blank line. */
		}

		if (*s == '[')
		{
			in_link = (strncmp(s, "[link]", 6) == 0);
			continue;
		}

		if (!in_link)
		{
			continue;
		}

		char *eq = strchr(s, '=');
		if (!eq)
		{
			continue;
		}

		*eq = '\0';
		char *key_end = eq;
		while (key_end > s && (key_end[-1] == ' ' || key_end[-1] == '\t'))
		{
			key_end--;
		}

		*key_end = '\0';
		const char *val = eq + 1;
		if (strcmp(s, "libs") == 0)
		{
			parse_string_array(val, (char *)cfg->libs, CFG_LIB_LEN, CFG_MAX_LIBS, &cfg->nlibs);
		}
		else if (strcmp(s, "lib_paths") == 0)
		{
			parse_string_array(val, (char *)cfg->lib_paths, CFG_PATH_LEN, CFG_MAX_PATHS, &cfg->nlib_paths);
		}
	}
}

void config_load(const char *src_arg, LinkConfig *cfg)
{
	char path[600];

	/* Derive the config path: <dir>/breezy.toml. If src_arg is a directory,
	   that is the dir; otherwise use the source file's parent (or "."). */
	DIR *d = opendir(src_arg);
	if (d)
	{
		closedir(d);
		snprintf(path, sizeof(path), "%s/breezy.toml", src_arg);
	}
	else
	{
		const char *slash = strrchr(src_arg, '/');
		const char *bslash = strrchr(src_arg, '\\');
		if (bslash > slash)
		{
			slash = bslash;
		}

		if (slash)
		{
			int len = (int)(slash - src_arg);
			snprintf(path, sizeof(path), "%.*s/breezy.toml", len, src_arg);
		}
		else
		{
			snprintf(path, sizeof(path), "breezy.toml");
		}
	}

	FILE *f = fopen(path, "rb");
	if (!f)
	{
		return;   /* No config -> silent no-op. */
	}

	static char buf[65536];
	size_t got = fread(buf, 1, sizeof(buf) - 1, f);
	buf[got] = '\0';
	fclose(f);

	config_parse_links(buf, cfg);
}
