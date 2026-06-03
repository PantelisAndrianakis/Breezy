#include "breezy.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#endif

extern char __vtable_IOException[];   /* Emitted per-program by codegen. */

static const char *g_io_error;        /* Set by a failed op; cleared by bzy_io_check. */

static void io_fail(const char *msg)
{
	g_io_error = msg;                 /* Static strings only (stable lifetime). */
}

/* Called by codegen after each fallible File op. Throws IOException if the last
   op failed; pc/frame locate the Breezy call site (the bzy_oob pattern). */
void bzy_io_check(int64_t pc, int64_t frame)
{
	if (!g_io_error)
	{
		return;
	}

	void *msg = bzy_str_new(g_io_error, (int64_t)strlen(g_io_error));
	g_io_error = NULL;
	void *exc = bzy_alloc(32);
	*(void**)exc = (void*)__vtable_IOException;
	*(void**)((char*)exc + 24) = msg;
	bzy_throw(exc, pc, frame);        /* Never returns. */
}

/* Returns 1 if path exists; sets *isdir (if non-NULL) to 1 for a directory. */
static int file_stat(const char *path, int *isdir)
{
#ifdef _WIN32
	DWORD a = GetFileAttributesA(path);
	if (a == INVALID_FILE_ATTRIBUTES)
	{
		return 0;
	}

	if (isdir)
	{
		*isdir = (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
	}

	return 1;
#else
	struct stat st;
	if (stat(path, &st) != 0)
	{
		return 0;
	}

	if (isdir)
	{
		*isdir = S_ISDIR(st.st_mode) ? 1 : 0;
	}

	return 1;
#endif
}

int64_t bzy_file_exists(void *path)
{
	return file_stat(bzy_str_data(path), NULL);
}

int64_t bzy_file_is_file(void *path)
{
	int d = 0;
	return file_stat(bzy_str_data(path), &d) && !d;
}

int64_t bzy_file_is_folder(void *path)
{
	int d = 0;
	return file_stat(bzy_str_data(path), &d) && d;
}

void bzy_file_create_file(void *path)
{
	FILE *f = fopen(bzy_str_data(path), "wb");
	if (!f)
	{
		io_fail("File.createFile: could not create file");
		return;
	}

	fclose(f);
}

/* mkdir -p: create each path component in turn. */
void bzy_file_create_folder(void *path)
{
	const char *p = bzy_str_data(path);
	char buf[1024];
	size_t n = strlen(p);
	if (n >= sizeof(buf))
	{
		io_fail("File.createFolder: path too long");
		return;
	}

	memcpy(buf, p, n + 1);
	for (size_t i = 1; i <= n; i++)
	{
		if (i == n || buf[i] == '/' || buf[i] == '\\')
		{
			char saved = buf[i];
			buf[i] = '\0';
#ifdef _WIN32
			if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
			{
				io_fail("File.createFolder: could not create folder");
				return;
			}
#else
			if (mkdir(buf, 0777) != 0 && errno != EEXIST)
			{
				io_fail("File.createFolder: could not create folder");
				return;
			}
#endif
			buf[i] = saved;
		}
	}
}

void bzy_file_delete(void *path)
{
	const char *p = bzy_str_data(path);
	int d = 0;
	if (!file_stat(p, &d))
	{
		io_fail("File.delete: path does not exist");
		return;
	}

#ifdef _WIN32
	int ok = d ? (RemoveDirectoryA(p) != 0) : (DeleteFileA(p) != 0);
#else
	int ok = d ? (rmdir(p) == 0) : (unlink(p) == 0);
#endif
	if (!ok)
	{
		io_fail("File.delete: could not delete path");
	}
}

#ifdef _WIN32
static int delete_tree(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	if (a == INVALID_FILE_ATTRIBUTES)
	{
		return 0;
	}

	if (!(a & FILE_ATTRIBUTE_DIRECTORY))
	{
		return DeleteFileA(path) != 0;
	}

	char pat[1024];
	snprintf(pat, sizeof(pat), "%s\\*", path);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
			{
				continue;
			}

			char child[1024];
			snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
			if (!delete_tree(child))
			{
				FindClose(h);
				return 0;
			}
		}
		while (FindNextFileA(h, &fd));
		FindClose(h);
	}

	return RemoveDirectoryA(path) != 0;
}
#else
static int delete_tree(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
	{
		return 0;
	}

	if (!S_ISDIR(st.st_mode))
	{
		return unlink(path) == 0;
	}

	DIR *dir = opendir(path);
	if (dir)
	{
		struct dirent *e;
		while ((e = readdir(dir)) != NULL)
		{
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			{
				continue;
			}

			char child[1024];
			snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
			if (!delete_tree(child))
			{
				closedir(dir);
				return 0;
			}
		}

		closedir(dir);
	}

	return rmdir(path) == 0;
}
#endif

void bzy_file_delete_recursive(void *path)
{
	const char *p = bzy_str_data(path);
	if (!file_stat(p, NULL))
	{
		io_fail("File.deleteRecursive: path does not exist");
		return;
	}

	if (!delete_tree(p))
	{
		io_fail("File.deleteRecursive: could not delete tree");
	}
}
