#include <stdio.h>

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: breezy <project-dir-or-file.bz>\n");
		return 1;
	}

	printf("Breezy compiler stub: %s\n", argv[1]);
	return 0;
}
