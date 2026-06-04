#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"
#include "prelude.h"
#include "config.h"

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

#define MAX_FILES 128
static int collect_files(const char *path, char paths[][512])
{
	int n=0;
	DIR *d=opendir(path);
	if (!d)
	{
		strncpy(paths[0],path,511);
		paths[0][511]='\0';
		return 1;
	}
	struct dirent *e;
	while ((e=readdir(d))!=NULL)
	{
		if (has_suffix(e->d_name,".bzy"))
		{
			if (n>=MAX_FILES)
			{
				fprintf(stderr,"Too many files.\n");
				exit(1);
			}
			snprintf(paths[n],512,"%s/%s",path,e->d_name);
			n++;
		}
	}
	closedir(d);
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
	memset(&cfg, 0, sizeof(cfg));
	const char *src_arg = NULL;
#ifdef _WIN32
	Target target = TARGET_WINDOWS;   /* Default to the build host. */
#else
	Target target = TARGET_LINUX;
#endif
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i],"--link")==0 && i+1 < argc)
		{
			if (cfg.nlibs < CFG_MAX_LIBS)
			{
				snprintf(cfg.libs[cfg.nlibs++],CFG_LIB_LEN,"%s",argv[++i]);
			}
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
	}

	if (!src_arg)
	{
		fprintf(stderr,"Usage: breezy <project-dir-or-file.bzy> [--link <lib>]... [--target linux|windows]\n");
		return 1;
	}

	config_load(src_arg, &cfg);   /* Merge libs/lib_paths from <project>/breezy.toml. */
	static char paths[MAX_FILES][512];
	int nfiles=collect_files(src_arg,paths);

	static Parser parsers[MAX_FILES];
	static Unit *units[MAX_FILES];

	/* The prelude (built-in vector classes etc.) compiles ahead of user files. */
	int np=BZY_PRELUDE_COUNT;
	int total=np+nfiles;
	if (total>MAX_FILES)
	{
		fprintf(stderr,"Too many files (prelude + sources).\n");
		exit(1);
	}

	for (int i=0; i<np; i++)
	{
		parser_init(&parsers[i],BZY_PRELUDE[i]);
		units[i]=parse_unit(&parsers[i]);
	}
	for (int i=0; i<nfiles; i++)
	{
		char *src=read_file(paths[i]);
		parser_init(&parsers[np+i],src);
		units[np+i]=parse_unit(&parsers[np+i]);
	}

	static TypeTable tt;   /* ~8 MB: in BSS, not on the stack (would overflow Linux's 8 MB default). */
	types_init(&tt);
	types_register_builtins(&tt);
	for (int i=0; i<total; i++)
	{
		types_register_unit_names(&tt,units[i]);
	}
	for (int i=0; i<total; i++)
	{
		types_register_unit_members(&tt,units[i]);
	}
	resolve_program(&tt,units,total);

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
	printf("Wrote out.asm\n");

	const char *nasm_cmd = (target == TARGET_LINUX)
						   ? "nasm -f elf64 out.asm -o out.obj"
						   : "nasm -f win64 out.asm -o out.obj";
	if (system(nasm_cmd)!=0)
	{
		fprintf(stderr,"Nasm failed.\n");
		return 1;
	}
	char link_cmd[2048];
	int off;
	if (target == TARGET_LINUX)
	{
		off = snprintf(link_cmd,sizeof(link_cmd),"gcc -no-pie out.obj -L. -l_breezy -lpthread -lm");
	}
	else
	{
		off = snprintf(link_cmd,sizeof(link_cmd),"gcc out.obj -L. -l_breezy -lws2_32 -lwinhttp");
	}
	for (int i = 0; i < cfg.nlib_paths; i++)
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -L%s",cfg.lib_paths[i]);
	}
	for (int i = 0; i < cfg.nlibs; i++)
	{
		off += snprintf(link_cmd+off,sizeof(link_cmd)-off," -l%s",cfg.libs[i]);
	}
	snprintf(link_cmd+off,sizeof(link_cmd)-off," -o out.exe");
	if (system(link_cmd)!=0)
	{
		fprintf(stderr,"Gcc link failed.\n");
		return 1;
	}
	printf("Built out.exe\n");

	ast_free_all();
	return 0;
}
