#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "parser.h"
#include "enums.h"
#include "generics.h"
#include "fieldinit.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"
#include "prelude.h"
#include "cycleinfo.h"
#include "config.h"
#include "grow.h"

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

/* Collect .bzy file paths under `path` into a freshly grown array of strings,
   returned through `out_paths`; returns the count. No fixed file cap. */
static int collect_files(const char *path, char ***out_paths)
{
	int n=0, cap=0;
	char **paths=NULL;
	DIR *d=opendir(path);
	if (!d)
	{
		paths=grow_ensure(paths,0,&cap,sizeof(*paths));
		paths[0]=malloc(512);
		strncpy(paths[0],path,511);
		paths[0][511]='\0';
		*out_paths=paths;
		return 1;
	}
	struct dirent *e;
	while ((e=readdir(d))!=NULL)
	{
		if (has_suffix(e->d_name,".bzy"))
		{
			paths=grow_ensure(paths,n,&cap,sizeof(*paths));
			paths[n]=malloc(512);
			snprintf(paths[n],512,"%s/%s",path,e->d_name);
			n++;
		}
	}
	closedir(d);
	*out_paths=paths;
	if (n==0)
	{
		fprintf(stderr,"No .bzy files in %s\n",path);
		exit(1);
	}
	return n;
}
int main(int argc, char *argv[])
{
	/* Collect --link <lib> flags; the first non-flag arg is the source path. */
	LinkConfig cfg;
	AppConfig  app_cfg;
	memset(&cfg, 0, sizeof(cfg));
	memset(&app_cfg, 0, sizeof(app_cfg));
	const char *src_arg = NULL;
	const char *out_arg = NULL;   /* Optional second positional: final executable path. */
#ifdef _WIN32
	Target target = TARGET_WINDOWS;   /* Default to the build host. */
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
		else if (!src_arg)
		{
			src_arg = argv[i];
		}
		else if (!out_arg)
		{
			out_arg = argv[i];
		}
	}

	if (!src_arg)
	{
		fprintf(stderr,"Usage: breezy <project-dir-or-file.bzy> [output] [--link <lib>]... [--target linux|windows]\n");
		return 1;
	}
	if (!out_arg)
	{
		out_arg = "out.exe";   /* Backward-compatible default. */
	}

	config_load(src_arg, &cfg, &app_cfg);   /* Merge libs/lib_paths and [app] info from <project>/breezy.toml. */
	char **paths=NULL;
	int nfiles=collect_files(src_arg,&paths);

	/* Read user sources up front so we can decide whether the Desktop GUI
	   prelude needs injecting before any parsing happens. The Desktop prelude
	   is added only when a user source references the `Desktop` identifier, so
	   non-GUI programs reserve none of its names and pull no GTK runtime. */
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

	/* The prelude (built-in vector classes etc.) compiles ahead of user files;
	   the Desktop prelude follows it only when Desktop is referenced. */
	int np=BZY_PRELUDE_COUNT;
	int ndesk=uses_desktop?BZY_DESKTOP_PRELUDE_COUNT:0;
	int total=np+ndesk+nfiles;
	Parser *parsers=malloc((size_t)total*sizeof(*parsers));
	int units_cap=0;
	Unit **units=grow_reserve(NULL, total, &units_cap, sizeof(*units));

	for (int i=0; i<np; i++)
	{
		parser_init(&parsers[i],BZY_PRELUDE[i]);
		units[i]=parse_unit(&parsers[i]);
	}
	for (int i=0; i<ndesk; i++)
	{
		parser_init(&parsers[np+i],BZY_DESKTOP_PRELUDE[i]);
		units[np+i]=parse_unit(&parsers[np+i]);
	}
	for (int i=0; i<nfiles; i++)
	{
		parser_init(&parsers[np+ndesk+i],srcs[i]);
		units[np+ndesk+i]=parse_unit(&parsers[np+ndesk+i]);
	}

	/* Lower enums to synthesized classes (base + per-constant subclasses) before
	   generics, so enum field/arg types that use generics still get instantiated. */
	enums_expand(&units,&total,&units_cap);

	/* Lower user generics: synthesize one ordinary class per (template, type-args)
	   tuple, rewrite applications, and drop templates. Grows `total` in place. */
	generics_expand(&units,&total,&units_cap);

	/* Lower instance-field initializers into constructor-body assignments
	   (synthesizing zero-arg constructors where needed). Must run before the
	   type table captures has_ctor and the constructor label from d->ctor. */
	fieldinit_expand(units,total);

	static TypeTable tt;   /* ~8 MB: in BSS, not on the stack (would overflow Linux's 8 MB default). */
	types_init(&tt);
	types_register_builtins(&tt);
	for (int i=0; i<total; i++)
	{
		types_register_unit_names(&tt,units[i]);
	}
	types_reserve_hashable(&tt);   /* Reserve slots 0/1 for record hashCode/equals, before any interface. */
	for (int i=0; i<total; i++)
	{
		types_register_interfaces(&tt,units[i]);   /* Reserve vtable slots [0..K) before members. */
	}
	types_register_all_members(&tt,units,total);   /* Classes parent-first: file order is filesystem-dependent. */
	resolve_program(&tt,units,total);

	/* Measurement only (I2c.1): report the static cycle-analysis coverage when
	   BZY_CYCLE_REPORT is set. No effect on compilation otherwise. */
	if (getenv("BZY_CYCLE_REPORT"))
	{
		CycleReport cr = cycle_analyze(&tt);
		cycle_report_print(&cr);
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

	/* Append a .bzy_app metadata section if any [app] fields are set.
	   Strings are emitted as byte sequences to avoid NASM quoting issues. */
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

	/* On Windows: if [app] icon or metadata is set, generate a resource file and
	   compile it with windres so the icon and VERSIONINFO are embedded in the exe. */
	int has_rc = (target == TARGET_WINDOWS)
	             && (app_cfg.icon[0] || app_cfg.name[0] || app_cfg.version[0]
	                 || app_cfg.description[0] || app_cfg.author[0]);
	if (has_rc)
	{
		FILE *rc = fopen("out.rc", "w");
		if (rc)
		{
			/* Icon resource: resolve relative icon path against the project dir. */
			if (app_cfg.icon[0])
			{
				char icon_path[700];
				snprintf(icon_path, sizeof(icon_path), "%s/%s",
				         app_cfg.project_dir[0] ? app_cfg.project_dir : ".",
				         app_cfg.icon);
				/* Forward slashes; windres accepts them on Windows. */
				for (char *cp = icon_path; *cp; cp++)
				{
					if (*cp == '\\')
					{
						*cp = '/';
					}
				}
				fprintf(rc, "1 ICON \"%s\"\n\n", icon_path);
			}

			/* VERSIONINFO: parse "major.minor.patch.build" from the version string. */
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
		/* -L. finds a co-located lib_breezy.a (distribution); -Lbuild/linux finds
		   the per-host build output when running from the project tree. */
		off = snprintf(link_cmd,sizeof(link_cmd),"gcc -no-pie out.obj -L. -Lbuild/linux -l_breezy -lpthread -lm -lcurl -ldl");
	}
	else
	{
		off = snprintf(link_cmd,sizeof(link_cmd),"gcc out.obj%s -L. -Lbuild/win -l_breezy -lws2_32 -lwinhttp",
		               has_rc ? " out_res.obj" : "");
	}
	for (int i = 0; i < cfg.nlib_paths; i++)
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -L%s",cfg.lib_paths[i]);
	}
	for (int i = 0; i < cfg.nlibs; i++)
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -l%s",cfg.libs[i]);
	}
	/* Strip symbols (-s): the emitted program carries no source-level debug info
	   and the runtime is opaque, so the only thing the linker would otherwise
	   embed is the toolchain CRT's own debug sections - ~3x the binary size for
	   no debugging value. */
	off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -s");
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
