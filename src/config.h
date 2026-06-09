#ifndef CONFIG_H
#define CONFIG_H

#define CFG_MAX_LIBS   64
#define CFG_LIB_LEN    64
#define CFG_MAX_PATHS  32
#define CFG_PATH_LEN   256

typedef struct
{
	char libs[CFG_MAX_LIBS][CFG_LIB_LEN];
	int  nlibs;
	char lib_paths[CFG_MAX_PATHS][CFG_PATH_LEN];
	int  nlib_paths;
} LinkConfig;

/* Parse the [link] section of a breezy.toml-style document held in `text`,
   appending any libs/lib_paths to `cfg`. A tolerant mini-reader, not full TOML. */
void config_parse_links(const char *text, LinkConfig *cfg);

/* Load <project>/breezy.toml (derived from `src_arg`: the directory itself if it
   is one, else the source file's parent) and parse its [link] section into `cfg`.
   A missing file is a silent no-op. */
void config_load(const char *src_arg, LinkConfig *cfg);

/* 1 if the experimental IR + register-allocator backend is enabled (env BZY_IR
   set to a non-empty, non-"0" value). Read once from the environment and cached.
   Default 0, so the existing emitter remains the default path. */
int bzy_ir_enabled(void);

#endif
