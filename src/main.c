#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#ifdef _WIN32
// Windows also declares TokenType, which is the lexer token type in this driver.
#define TokenType WindowsTokenType
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef TokenType
#else
#include <sys/stat.h>
#endif
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "fieldinit.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"
#include "cemit.h"
#include "prelude.h"
#include "config.h"
#include "grow.h"
#include "lsp.h"
#include "symbols.h"

static char *read_file(const char *path)
{
	FILE *f=fopen(path,"rb");
	if (!f)
	{
		perror(path);
		exit(1);
	}

	fseek(f,0,SEEK_END);
	long sz=ftell(f);
	rewind(f);
	char *buf=malloc(sz+1);
	if (fread(buf,1,sz,f)!=(size_t)sz)
	{
		perror("read");
		exit(1);
	}

	buf[sz]='\0';
	fclose(f);
	return buf;
}

static int has_suffix(const char *s, const char *suf)
{
	size_t ls=strlen(s),lf=strlen(suf);
	return ls>=lf && strcmp(s+ls-lf,suf)==0;
}

/**
 * Linked entries are skipped so directory cycles cannot revisit a project.
 */
static void collect_directory(const char *path, DIR *directory, char ***out_paths, int *count, int *capacity)
{
	while (1)
	{
		errno=0;
		struct dirent *entry=readdir(directory);
		if (!entry)
		{
			if (errno!=0)
			{
				perror(path);
				exit(1);
			}

			break;
		}

		if (strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0)
		{
			continue;
		}

		size_t pathLength=strlen(path)+strlen(entry->d_name)+2;
		char *entryPath=malloc(pathLength);
		if (!entryPath)
		{
			fprintf(stderr,"Out of memory collecting source paths.\n");
			exit(1);
		}

		snprintf(entryPath,pathLength,"%s/%s",path,entry->d_name);
		int isDirectory=0;
		int isFile=0;
#ifdef _WIN32
		DWORD attributes=GetFileAttributesA(entryPath);
		if (attributes==INVALID_FILE_ATTRIBUTES)
		{
			fprintf(stderr,"Cannot inspect %s (Windows error %lu).\n",entryPath,(unsigned long)GetLastError());
			exit(1);
		}

		if ((attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0)
		{
			free(entryPath);
			continue;
		}

		isDirectory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
		isFile=!isDirectory;
#else
		struct stat attributes;
		if (lstat(entryPath,&attributes)!=0)
		{
			perror(entryPath);
			exit(1);
		}

		isDirectory=S_ISDIR(attributes.st_mode);
		isFile=S_ISREG(attributes.st_mode);
#endif
		if (isDirectory)
		{
			DIR *child=opendir(entryPath);
			if (!child)
			{
				perror(entryPath);
				exit(1);
			}

			collect_directory(entryPath,child,out_paths,count,capacity);
			closedir(child);
		}
		else if (isFile)
		{
			if (has_suffix(entry->d_name,".bzy"))
			{
				*out_paths=grow_ensure(*out_paths,*count,capacity,sizeof(**out_paths));
				(*out_paths)[*count]=entryPath;
				(*count)++;
				continue;
			}
		}

		free(entryPath);
	}
}

/**
 * Collect .bzy files recursively, or preserve an explicitly supplied source file.
 */
static int collect_files(const char *path, char ***out_paths)
{
	int count=0;
	int capacity=0;
	char **paths=NULL;
	DIR *directory=opendir(path);
	if (!directory)
	{
		paths=grow_ensure(paths,0,&capacity,sizeof(*paths));
		paths[0]=malloc(strlen(path)+1);
		if (!paths[0])
		{
			fprintf(stderr,"Out of memory collecting source paths.\n");
			exit(1);
		}

		strcpy(paths[0],path);
		*out_paths=paths;
		return 1;
	}

	collect_directory(path,directory,&paths,&count,&capacity);
	closedir(directory);
	*out_paths=paths;
	if (count==0)
	{
		fprintf(stderr,"No .bzy files in %s\n",path);
		exit(1);
	}

	return count;
}

int main(int argc, char *argv[])
{
	// Collect --link <lib> flags; the first non-flag arg is the source path.
	LinkConfig cfg;
	AppConfig  app_cfg;
	memset(&cfg, 0, sizeof(cfg));
	memset(&app_cfg, 0, sizeof(app_cfg));
	const char *src_arg = NULL;
	const char *out_arg = NULL; // Optional second positional: final executable path.
	int check_only = 0; // --check: parse + resolve, emit JSON diagnostics, no codegen.
	int lsp_mode = 0; // --lsp: run the stdio language server, no compile.
	int symbols_mode = 0; // --symbols: parse + resolve, emit the JSON symbol index, no codegen.
#ifdef _WIN32
	Target target = TARGET_WINDOWS; // Default to the build host.
#else
	Target target = TARGET_LINUX;
#endif
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i],"--link")==0 && i+1 < argc)
		{
			cfg.libs = grow_ensure(cfg.libs, cfg.nlibs, &cfg.libs_cap, sizeof(*cfg.libs));
			cfg.libs[cfg.nlibs] = malloc(CFG_LIB_LEN);
			snprintf(cfg.libs[cfg.nlibs], CFG_LIB_LEN, "%s", argv[++i]);
			cfg.nlibs++;
		}
		else if (strcmp(argv[i],"--target")==0 && i+1 < argc)
		{
			const char *t = argv[++i];
			if (strcmp(t,"linux")==0)
			{
				target = TARGET_LINUX;
			}
			else if (strcmp(t,"windows")==0)
			{
				target = TARGET_WINDOWS;
			}
			else
			{
				fprintf(stderr,"Unknown --target '%s' (use linux|windows).\n", t);
				return 1;
			}
		}
		else if (strcmp(argv[i],"--check")==0)
		{
			check_only = 1;
			lexer_diag_json(1);
		}
		else if (strcmp(argv[i],"--lsp")==0)
		{
			lsp_mode = 1;
		}
		else if (strcmp(argv[i],"--symbols")==0)
		{
			symbols_mode = 1;
			lexer_diag_json(1); // Stay quiet on diagnostics: a parse/resolve error exits before the index.
		}
		else if (!src_arg)
		{
			src_arg = argv[i];
		}
		else if (!out_arg)
		{
			out_arg = argv[i];
		}
	}

	// --lsp: hand off to the stdio language server; it has no source positional.
	if (lsp_mode)
	{
		return lsp_main(argv[0]);
	}

	if (!src_arg)
	{
		fprintf(stderr,"Usage: breezy <project-dir-or-file.bzy> [output] [--link <lib>]... [--target linux|windows]\n");
		return 1;
	}

	if (!out_arg)
	{
		out_arg = "out.exe"; // Backward-compatible default.
	}

	config_load(src_arg, &cfg, &app_cfg); // Merge libs/lib_paths and [app] info from <project>/breezy.toml.
	char **paths=NULL;
	int nfiles=collect_files(src_arg,&paths);

	// Read user sources up front so we can decide whether the Desktop GUI prelude needs injecting before any parsing happens.
	// The Desktop prelude is added only when a user source references the `Desktop` identifier, so non-GUI programs reserve none of its names and pull no GTK runtime.
	char **srcs=malloc((size_t)nfiles*sizeof(*srcs));
	int uses_desktop=0;
	for (int i=0; i<nfiles; i++)
	{
		srcs[i]=read_file(paths[i]);
		if (!uses_desktop)
		{
			const char *p=srcs[i];
			while ((p=strstr(p,"Desktop"))!=NULL)
			{
				char before=(p==srcs[i])?' ':p[-1];
				char after=p[7];
				int b_ok=!(isalnum((unsigned char)before)||before=='_');
				int a_ok=!(isalnum((unsigned char)after)||after=='_');
				if (b_ok && a_ok)
				{
					uses_desktop=1;
					break;
				}

				p+=7;
			}
		}
	}

	// The prelude (built-in vector classes etc.) compiles ahead of user files; the Desktop prelude follows it only when Desktop is referenced.
	int np=BZY_PRELUDE_COUNT;
	int ndesk=uses_desktop?BZY_DESKTOP_PRELUDE_COUNT:0;
	int total=np+ndesk+nfiles;
	Parser *parsers=malloc((size_t)total*sizeof(*parsers));
	int units_cap=0;
	Unit **units=grow_reserve(NULL, total, &units_cap, sizeof(*units));

	for (int i=0; i<np; i++)
	{
		parser_init(&parsers[i],BZY_PRELUDE[i]);
		parsers[i].is_prelude = 1;
		units[i]=parse_unit(&parsers[i]);
		units[i]->is_prelude=1;
	}

	for (int i=0; i<ndesk; i++)
	{
		parser_init(&parsers[np+i],BZY_DESKTOP_PRELUDE[i]);
		parsers[np+i].is_prelude = 1;
		units[np+i]=parse_unit(&parsers[np+i]);
		units[np+i]->is_prelude=1;
	}

	for (int i=0; i<nfiles; i++)
	{
		parser_init(&parsers[np+ndesk+i],srcs[i]);
		parsers[np+ndesk+i].lex.file=paths[i]; // Real path for diagnostics.
		units[np+ndesk+i]=parse_unit(&parsers[np+ndesk+i]);
	}

	// Lower enums to synthesized classes (base + per-constant subclasses) before generics, so enum field/arg types that use generics still get instantiated.
	enums_expand(&units,&total,&units_cap);

	// Lower user generics: synthesize one ordinary class per (template, type-args) tuple, rewrite applications, and drop templates.
	// Grows `total` in place.
	generics_expand(&units,&total,&units_cap);

	// Lower instance-field initializers into constructor-body assignments (synthesizing zero-arg constructors where needed).
	// Must run before the type table captures has_ctor and the constructor label from d->ctor.
	fieldinit_expand(units,total);

	static TypeTable tt; // ~8 MB: in BSS, not on the stack (would overflow Linux's 8 MB default).
	types_init(&tt);
	types_register_builtins(&tt);
	for (int i=0; i<total; i++)
	{
		types_register_unit_names(&tt,units[i]);
	}

	types_reserve_hashable(&tt); // Reserve slots 0/1 for record hashCode/equals, before any interface.
	for (int i=0; i<total; i++)
	{
		types_register_interfaces(&tt,units[i]); // Reserve vtable slots [0..K) before members.
	}

	types_register_all_members(&tt,units,total); // Classes parent-first: file order is filesystem-dependent.
	resolve_program(&tt,units,total);

	// --symbols: front-end only.
	// Reaching here means resolve succeeded; emit the LSP symbol index and stop before codegen.
	if (symbols_mode)
	{
		symbols_emit(units,total);
		return 0;
	}

	// --check: front-end only. Reaching here means no diagnostics fired.
	if (check_only)
	{
		printf("{\"ok\":true}\n");
		return 0;
	}

	FILE *out=fopen("out.asm","w");
	if (!out)
	{
		perror("out.asm");
		exit(1);
	}

	Codegen cg;
	cg_init(&cg,out);
	cg.target = target;
	cg_program(&cg,&tt,units,total);
	fclose(out);

	// Append a .bzy_app metadata section if any [app] fields are set.
	// Strings are emitted as byte sequences to avoid NASM quoting issues.
	const char *app_symbol = NULL;
	int has_app_info = app_cfg.name[0] || app_cfg.version[0]
					   || app_cfg.description[0] || app_cfg.author[0];
	if (has_app_info)
	{
		FILE *af = fopen("out.asm", "a");
		if (af)
		{
			if (target == TARGET_LINUX)
			{
				fprintf(af, "\nsection .bzy_app\n");
			}
			else
			{
				fprintf(af, "\nsection .bzy_app data\n");
			}

			const char *fields[4][2] =
			{
				{ "bzy_app_name",        app_cfg.name        },
				{ "bzy_app_version",     app_cfg.version     },
				{ "bzy_app_author",      app_cfg.author      },
				{ "bzy_app_description", app_cfg.description },
			};
			for (int fi = 0; fi < 4; fi++)
			{
				if (!fields[fi][1][0])
				{
					continue;
				}

				app_symbol = fields[fi][0];
				fprintf(af, "global %s\n%s: db ", fields[fi][0], fields[fi][0]);
				for (const char *cp = fields[fi][1]; *cp; cp++)
				{
					fprintf(af, "%d,", (unsigned char)*cp);
				}

				fprintf(af, "0\n");
			}

			fclose(af);
		}
	}

	printf("Wrote out.asm\n");

	const char *nasm_cmd = (target == TARGET_LINUX)
						   ? "nasm -f elf64 out.asm -o out.obj"
						   : "nasm -f win64 out.asm -o out.obj";
	if (system(nasm_cmd)!=0)
	{
		fprintf(stderr,"Nasm failed.\n");
		return 1;
	}

	// C route: compile the generated C units, if any, into out_c.obj.
	int has_c = cemit_source() != NULL;
	if (has_c)
	{
		FILE *cf = fopen("out_c.c", "w");
		if (!cf)
		{
			perror("out_c.c");
			return 1;
		}

		fputs(cemit_source(), cf);
		fclose(cf);
		printf("Wrote out_c.c\n");
		// -falign-jumps=64 starts every jump target (each case of a dispatch loop) on a cache line: interp-style loops measured 10-25% faster on both platforms with no loss elsewhere, for a few hundred bytes of padding.
		if (system("gcc -O3 -falign-jumps=64 -fwrapv -fno-strict-aliasing -ffp-contract=off -c out_c.c -o out_c.obj") != 0)
		{
			fprintf(stderr, "Compiling out_c.c failed.\n");
			return 1;
		}
	}

	// On Windows: if [app] icon or metadata is set, generate a resource file and compile it with windres so the icon and VERSIONINFO are embedded in the exe.
	int has_rc = (target == TARGET_WINDOWS)
				 && (app_cfg.icon[0] || app_cfg.name[0] || app_cfg.version[0]
					 || app_cfg.description[0] || app_cfg.author[0]);
	if (has_rc)
	{
		FILE *rc = fopen("out.rc", "w");
		if (rc)
		{
			// Icon resource: resolve relative icon path against the project dir.
			if (app_cfg.icon[0])
			{
				char icon_path[700];
				snprintf(icon_path, sizeof(icon_path), "%s/%s",
						 app_cfg.project_dir[0] ? app_cfg.project_dir : ".",
						 app_cfg.icon);
				// Forward slashes; windres accepts them on Windows.
				for (char *cp = icon_path; *cp; cp++)
				{
					if (*cp == '\\')
					{
						*cp = '/';
					}
				}

				fprintf(rc, "1 ICON \"%s\"\n\n", icon_path);
			}

			// VERSIONINFO: parse "major.minor.patch.build" from the version string.
			int v0 = 0, v1 = 0, v2 = 0, v3 = 0;
			if (app_cfg.version[0])
			{
				sscanf(app_cfg.version, "%d.%d.%d.%d", &v0, &v1, &v2, &v3);
			}

			fprintf(rc,
					"VS_VERSION_INFO VERSIONINFO\n"
					" FILEVERSION %d,%d,%d,%d\n"
					" PRODUCTVERSION %d,%d,%d,%d\n"
					" FILEFLAGSMASK 0x3fL\n"
					" FILEFLAGS 0x0L\n"
					" FILEOS 0x40004L\n"
					" FILETYPE 0x1L\n"
					" FILESUBTYPE 0x0L\n"
					"BEGIN\n"
					"    BLOCK \"StringFileInfo\"\n"
					"    BEGIN\n"
					"        BLOCK \"040904b0\"\n"
					"        BEGIN\n",
					v0, v1, v2, v3, v0, v1, v2, v3);

			if (app_cfg.name[0])
			{
				fprintf(rc, "            VALUE \"ProductName\", \"%s\"\n", app_cfg.name);
			}

			if (app_cfg.version[0])
			{
				fprintf(rc, "            VALUE \"ProductVersion\", \"%s\"\n", app_cfg.version);
				fprintf(rc, "            VALUE \"FileVersion\", \"%s\"\n", app_cfg.version);
			}

			if (app_cfg.description[0])
			{
				fprintf(rc, "            VALUE \"FileDescription\", \"%s\"\n", app_cfg.description);
			}

			if (app_cfg.author[0])
			{
				fprintf(rc, "            VALUE \"LegalCopyright\", \"%s\"\n", app_cfg.author);
			}

			fprintf(rc,
					"        END\n"
					"    END\n"
					"    BLOCK \"VarFileInfo\"\n"
					"    BEGIN\n"
					"        VALUE \"Translation\", 0x0409, 1200\n"
					"    END\n"
					"END\n");
			fclose(rc);

			if (system("windres out.rc -o out_res.obj") != 0)
			{
				fprintf(stderr, "Warning: windres failed; icon and version info will not be embedded.\n");
				has_rc = 0;
			}
		}
		else
		{
			has_rc = 0;
		}
	}

	char link_cmd[2048];
	int off;
	if (target == TARGET_LINUX)
	{
		// -L. finds a co-located lib_breezy.a (distribution); -Lbuild/linux finds the per-host build output when running from the project tree.
		off = snprintf(link_cmd,sizeof(link_cmd),"gcc -no-pie -Wl,--gc-sections,--as-needed out.obj%s -L. -Lbuild/linux -l_breezy -lpthread -lm -lcurl -ldl",
					   has_c ? " out_c.obj" : "");
	}
	else
	{
		off = snprintf(link_cmd,sizeof(link_cmd),"gcc -Wl,--gc-sections out.obj%s%s -L. -Lbuild/win -l_breezy -lws2_32 -lwinhttp",
					   has_c ? " out_c.obj" : "", has_rc ? " out_res.obj" : "");
	}

	for (int i = 0; i < cfg.nlib_paths; i++)
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -L%s",cfg.lib_paths[i]);
	}

	for (int i = 0; i < cfg.nlibs; i++)
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -l%s",cfg.libs[i]);
	}
	if (app_symbol)
	{
		// Application metadata has no code references, so retain its section explicitly.
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -Wl,--undefined=%s",app_symbol);
	}

	// Strip symbols (-s): the emitted program carries no source-level debug info and the runtime is opaque, so the only thing the linker would otherwise embed is the toolchain CRT's own debug sections - ~3x the binary size for no debugging value.
	off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -s");
	// BZY_LINK_MAP: emit a linker map (out.map) listing which runtime archive members were pulled in.
	// Used by the no-bloat regression gate to prove that an unused feature's object is dropped, never linked.
	// No effect unset.
	if (getenv("BZY_LINK_MAP"))
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -Wl,-Map=out.map");
	}

	snprintf(link_cmd+off,sizeof(link_cmd)-off," -o %s",out_arg);
	if (system(link_cmd)!=0)
	{
		fprintf(stderr,"Gcc link failed.\n");
		return 1;
	}

	printf("Built %s\n",out_arg);

	ast_free_all();
	return 0;
}
