#ifndef CONFIG_H
#define CONFIG_H

#define CFG_LIB_LEN      64
#define CFG_PATH_LEN    256
#define CFG_APP_STR_LEN 256

typedef struct
{
	char **libs;       /* Each entry up to CFG_LIB_LEN bytes; grown dynamically. */
	int    nlibs;
	int    libs_cap;
	char **lib_paths;  /* Each entry up to CFG_PATH_LEN bytes. */
	int    nlib_paths;
	int    lib_paths_cap;
} LinkConfig;

typedef struct
{
	char name[CFG_APP_STR_LEN];
	char version[CFG_APP_STR_LEN];
	char description[CFG_APP_STR_LEN];
	char author[CFG_APP_STR_LEN];
	char icon[CFG_PATH_LEN];        /* .ico path relative to breezy.toml dir. */
	char project_dir[CFG_PATH_LEN]; /* Resolved dir; used to expand icon to absolute path. */
} AppConfig;

/* Parse the [link] section of a breezy.toml-style document held in `text`,
   appending any libs/lib_paths to `cfg`. A tolerant mini-reader, not full TOML. */
void config_parse_links(const char *text, LinkConfig *cfg);

/* Parse the [app] section of a breezy.toml-style document held in `text`,
   filling `app` with name/version/description/author/icon. */
void config_parse_app(const char *text, AppConfig *app);

/* Load <project>/breezy.toml (derived from `src_arg`: the directory itself if it
   is one, else the source file's parent) and parse its sections.
   Either pointer may be NULL to skip that section. A missing file is a silent no-op. */
void config_load(const char *src_arg, LinkConfig *link, AppConfig *app);

/* 1 if the IR + register-allocator backend is enabled. Read once from the
   environment and cached. Default ON for eligible functions; set BZY_IR=0 to
   force the emitter everywhere. */
int bzy_ir_enabled(void);

/* 1 if IR loop regions are enabled (Plan 4): eligible loop nests inside emitter
   functions are emitted from IR. Default ON; set BZY_IR_REGIONS=0 to keep loops
   on the emitter while leaving whole-function IR active. Always 0 when the IR
   backend itself is disabled. */
int bzy_ir_regions_enabled(void);

/* Discard the cached env flags so the next bzy_ir_*_enabled() call re-reads the
   environment. Used by tests to toggle BZY_IR around a single case. */
void bzy_config_reset_cache(void);

#endif
