#include "config.h"
#include "grow.h"
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

int bzy_ir_regions_enabled(void)
{
	static int cached = -1;
	if (cached < 0)
	{
		/* Default ON. Set BZY_IR_REGIONS=0 to keep eligible loops on the emitter
		   while leaving whole-function IR active; BZY_IR=0 disables both. */
		const char *v = getenv("BZY_IR_REGIONS");
		cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
	}

	return cached && bzy_ir_enabled();
}

/* Append each double-quoted token in `val` to the grown string array `*arr`
   (each entry allocated `width` bytes), bumping *count and growing *cap. Tokens
   longer than the entry are truncated. */
static void parse_string_array(const char *val, char ***arr, int width, int *count, int *cap)
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
			{
				*arr = grow_ensure(*arr, *count, cap, sizeof(**arr));
				char *slot = malloc(width);
				if (len >= width)
				{
					len = width - 1;
				}

				memcpy(slot, start, len);
				slot[len] = '\0';
				(*arr)[(*count)++] = slot;
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
			parse_string_array(val, &cfg->libs, CFG_LIB_LEN, &cfg->nlibs, &cfg->libs_cap);
		}
		else if (strcmp(s, "lib_paths") == 0)
		{
			parse_string_array(val, &cfg->lib_paths, CFG_PATH_LEN, &cfg->nlib_paths, &cfg->lib_paths_cap);
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
