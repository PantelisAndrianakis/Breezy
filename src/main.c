#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "parser.h"
#include "types.h"
#include "resolve.h"
#include "codegen.h"

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
				fprintf(stderr,"too many files\n");
				exit(1);
			}
			snprintf(paths[n],512,"%s/%s",path,e->d_name);
			n++;
		}
	}
	closedir(d);
	if (n==0)
	{
		fprintf(stderr,"no .bzy files in %s\n",path);
		exit(1);
	}
	return n;
}
int main(int argc, char *argv[])
{
	if (argc<2)
	{
		fprintf(stderr,"Usage: breezy <project-dir-or-file.bzy>\n");
		return 1;
	}
	static char paths[MAX_FILES][512];
	int nfiles=collect_files(argv[1],paths);

	static Parser parsers[MAX_FILES];
	static Unit *units[MAX_FILES];
	for (int i=0; i<nfiles; i++)
	{
		char *src=read_file(paths[i]);
		parser_init(&parsers[i],src);
		units[i]=parse_unit(&parsers[i]);
	}

	TypeTable tt;
	types_init(&tt);
	for (int i=0; i<nfiles; i++) types_register_unit_names(&tt,units[i]);
	for (int i=0; i<nfiles; i++) types_register_unit_members(&tt,units[i]);
	resolve_program(&tt,units,nfiles);

	FILE *out=fopen("out.asm","w");
	if (!out)
	{
		perror("out.asm");
		exit(1);
	}
	Codegen cg;
	cg_init(&cg,out);
	cg_program(&cg,&tt,units,nfiles);
	fclose(out);
	printf("Wrote out.asm\n");

	if (system("nasm -f win64 out.asm -o out.obj")!=0)
	{
		fprintf(stderr,"nasm failed\n");
		return 1;
	}
	if (system("gcc out.obj -L. -l_breezy -o out.exe")!=0)
	{
		fprintf(stderr,"gcc link failed\n");
		return 1;
	}
	printf("Built out.exe\n");

	ast_free_all();
	return 0;
}
