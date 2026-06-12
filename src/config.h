#ifndef CONFIG_H
#define CONFIG_H

#define CFG_LIB_LEN    64
#define CFG_PATH_LEN   256

typedef struct
{
	char **libs;       /* Each entry up to CFG_LIB_LEN bytes; grown dynamically. */
	int    nlibs;
	int    libs_cap;
	char **lib_paths;  /* Each entry up to CFG_PATH_LEN bytes. */
	int    nlib_paths;
	int    lib_paths_cap;
} LinkConfig;

/* Parse the [link] section of a breezy.toml-style document held in `text`,
   appending any libs/lib_paths to `cfg`. A tolerant mini-reader, not full TOML. */
void config_parse_links(const char *text, LinkConfig *cfg);

/* Load <project>/breezy.toml (derived from `src_arg`: the directory itself if it
   is one, else the source file's parent) and parse its [link] section into `cfg`.
   A missing file is a silent no-op. */
void config_load(const char *src_arg, LinkConfig *cfg);

/* 1 if the IR + register-allocator backend is enabled. Read once from the
   environment and cached. Default ON for eligible functions; set BZY_IR=0 to
   force the emitter everywhere. */
int bzy_ir_enabled(void);

/* 1 if IR loop regions are enabled (Plan 4): eligible loop nests inside emitter
   functions are emitted from IR. Default ON; set BZY_IR_REGIONS=0 to keep loops
   on the emitter while leaving whole-function IR active. Always 0 when the IR
   backend itself is disabled. */
int bzy_ir_regions_enabled(void);

#endif
