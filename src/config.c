#include "config.h"
#include "grow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* Env-derived flags, read once and cached (-1 = unread). bzy_config_reset_cache
   forces a re-read, which tests use to flip BZY_IR around a single case. */
static int g_ir_cached = -1;
static int g_ir_regions_cached = -1;

void bzy_config_reset_cache(void)
{
	g_ir_cached = -1;
	g_ir_regions_cached = -1;
}

int bzy_ir_enabled(void)
{
	if (g_ir_cached < 0)
	{
		/* Default ON: eligible call-free functions use the IR backend (the hot-spill
		   safety gate keeps any function the IR would lose on the emitter). Set
		   BZY_IR=0 to force the emitter everywhere. */
		const char *v = getenv("BZY_IR");
		g_ir_cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
	}

	return g_ir_cached;
}

int bzy_ir_regions_enabled(void)
{
	if (g_ir_regions_cached < 0)
	{
		/* Default ON. Set BZY_IR_REGIONS=0 to keep eligible loops on the emitter
		   while leaving whole-function IR active; BZY_IR=0 disables both. */
		const char *v = getenv("BZY_IR_REGIONS");
		g_ir_regions_cached = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
	}

	return g_ir_regions_cached && bzy_ir_enabled();
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

/* Copy the first double-quoted token in `val` into `out` (fixed buffer of `out_size` bytes). */
static void parse_string_value(const char *val, char *out, int out_size)
{
	const char *p = val;
	while (*p && *p != '"')
	{
		p++;
	}
	if (*p != '"')
	{
		return;
	}
	const char *start = ++p;
	while (*p && *p != '"')
	{
		p++;
	}
	int len = (int)(p - start);
	if (len >= out_size)
	{
		len = out_size - 1;
	}
	memcpy(out, start, len);
	out[len] = '\0';
}

/* Scan an INI-style body line by line. Within the named section header (e.g.
   "[app]"), each `key = value` line is trimmed and handed to `fn(key, val, ctx)`.
   Comments (#), blanks, and lines outside the section are skipped. */
static void config_scan(const char *text, const char *section,
						void (*fn)(const char *, const char *, void *), void *ctx)
{
	size_t seclen = strlen(section);
	int in_section = 0;
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
			in_section = (strncmp(s, section, seclen) == 0);
			continue;
		}

		if (!in_section)
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
		fn(s, eq + 1, ctx);
	}
}

static void app_kv(const char *key, const char *val, void *ctx)
{
	AppConfig *app = ctx;
	if (strcmp(key, "name") == 0)
	{
		parse_string_value(val, app->name, CFG_APP_STR_LEN);
	}
	else if (strcmp(key, "version") == 0)
	{
		parse_string_value(val, app->version, CFG_APP_STR_LEN);
	}
	else if (strcmp(key, "description") == 0)
	{
		parse_string_value(val, app->description, CFG_APP_STR_LEN);
	}
	else if (strcmp(key, "author") == 0)
	{
		parse_string_value(val, app->author, CFG_APP_STR_LEN);
	}
	else if (strcmp(key, "icon") == 0)
	{
		parse_string_value(val, app->icon, CFG_PATH_LEN);
	}
}

static void link_kv(const char *key, const char *val, void *ctx)
{
	LinkConfig *cfg = ctx;
	if (strcmp(key, "libs") == 0)
	{
		parse_string_array(val, &cfg->libs, CFG_LIB_LEN, &cfg->nlibs, &cfg->libs_cap);
	}
	else if (strcmp(key, "lib_paths") == 0)
	{
		parse_string_array(val, &cfg->lib_paths, CFG_PATH_LEN, &cfg->nlib_paths, &cfg->lib_paths_cap);
	}
}

void config_parse_app(const char *text, AppConfig *app)
{
	config_scan(text, "[app]", app_kv, app);
}

void config_parse_links(const char *text, LinkConfig *cfg)
{
	config_scan(text, "[link]", link_kv, cfg);
}

void config_load(const char *src_arg, LinkConfig *link, AppConfig *app)
{
	char path[600];
	char dir[CFG_PATH_LEN];

	/* Derive the config path: <dir>/breezy.toml. If src_arg is a directory,
	   that is the dir; otherwise use the source file's parent (or "."). */
	DIR *d = opendir(src_arg);
	if (d)
	{
		closedir(d);
		snprintf(path, sizeof(path), "%s/breezy.toml", src_arg);
		snprintf(dir, sizeof(dir), "%s", src_arg);
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
			snprintf(dir, sizeof(dir), "%.*s", len, src_arg);
		}
		else
		{
			snprintf(path, sizeof(path), "breezy.toml");
			snprintf(dir, sizeof(dir), ".");
		}
	}

	if (app)
	{
		snprintf(app->project_dir, sizeof(app->project_dir), "%s", dir);
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

	if (link)
	{
		config_parse_links(buf, link);
	}
	if (app)
	{
		config_parse_app(buf, app);
	}
}
