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

static __thread const char *g_io_error;   /* Per-thread: an offload worker sets its own; the wrapper transfers it. */

static void io_fail(const char *msg)
{
	g_io_error = msg;                 /* Static strings only (stable lifetime). */
}

/* Public wrapper so other runtime TUs (filechannel.c) can set the same thread-local
   io-error that bzy_io_check consumes. */
void bzy_io_fail(const char *msg)
{
	io_fail(msg);
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
		io_fail("File.createFile: could not create file.");
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
		io_fail("File.createFolder: path too long.");
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
				io_fail("File.createFolder: could not create folder.");
				return;
			}
#else
			if (mkdir(buf, 0777) != 0 && errno != EEXIST)
			{
				io_fail("File.createFolder: could not create folder.");
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
		io_fail("File.delete: path does not exist.");
		return;
	}

#ifdef _WIN32
	int ok = d ? (RemoveDirectoryA(p) != 0) : (DeleteFileA(p) != 0);
#else
	int ok = d ? (rmdir(p) == 0) : (unlink(p) == 0);
#endif
	if (!ok)
	{
		io_fail("File.delete: could not delete path.");
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
		io_fail("File.deleteRecursive: path does not exist.");
		return;
	}

	if (!delete_tree(p))
	{
		io_fail("File.deleteRecursive: could not delete tree.");
	}
}

/* Read the whole file into a malloc'd buffer; *out_len gets the byte count.
   Returns NULL (and sets io_fail) on error. The buffer is NUL-terminated. */
static char *read_all(const char *path, int64_t *out_len, const char *who)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		io_fail(who);
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0)
	{
		fclose(f);
		io_fail(who);
		return NULL;
	}

	char *buf = (char*)malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	*out_len = (int64_t)got;
	return buf;
}

static void *real_read_text(void *path)
{
	int64_t n = 0;
	char *buf = read_all(bzy_str_data(path), &n, "File.readText: could not read file.");
	if (!buf)
	{
		return NULL;
	}

	void *s = bzy_str_new(buf, n);
	free(buf);
	return s;
}

static void *real_read_lines(void *path)
{
	int64_t n = 0;
	char *buf = read_all(bzy_str_data(path), &n, "File.readLines: could not read file.");
	if (!buf)
	{
		return NULL;
	}

	int64_t lines = 0;
	for (int64_t i = 0; i < n; i++)
	{
		if (buf[i] == '\n')
		{
			lines++;
		}
	}

	if (n > 0 && buf[n - 1] != '\n')
	{
		lines++;   /* Final segment with no trailing newline. */
	}

	void *arr = bzy_array_new(lines, 1);
	void **elems = (void**)((char*)arr + 32);
	int64_t start = 0, idx = 0;
	for (int64_t i = 0; i <= n; i++)
	{
		if (i == n || buf[i] == '\n')
		{
			if (i == n && start == i)
			{
				break;   /* No dangling empty line after a trailing newline. */
			}

			int64_t end = i;
			if (end > start && buf[end - 1] == '\r')
			{
				end--;   /* Strip CR for CRLF line endings. */
			}

			elems[idx++] = bzy_str_new(buf + start, end - start);   /* Owned; transferred. */
			start = i + 1;
		}
	}

	free(buf);
	return arr;
}

static void write_file(void *path, void *content, const char *mode, const char *who)
{
	FILE *f = fopen(bzy_str_data(path), mode);
	if (!f)
	{
		io_fail(who);
		return;
	}

	int64_t len = bzy_str_len(content);
	if (len > 0)
	{
		fwrite(bzy_str_data(content), 1, (size_t)len, f);
	}

	fclose(f);
}

static void real_write_text(void *path, void *content)
{
	write_file(path, content, "wb", "File.writeText: could not write file.");
}

static void real_append_text(void *path, void *content)
{
	write_file(path, content, "ab", "File.appendText: could not write file.");
}

static void *real_read_bytes(void *path)
{
	int64_t n = 0;
	char *buf = read_all(bzy_str_data(path), &n, "File.readBytes: could not read file.");
	if (!buf)
	{
		return NULL;
	}

	void *arr = bzy_array_new(n, 0);   /* Value array: one byte per 8-byte slot. */
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int64_t i = 0; i < n; i++)
	{
		slots[i] = (unsigned char)buf[i];
	}

	free(buf);
	return arr;
}

static void real_write_bytes(void *path, void *data)
{
	FILE *f = fopen(bzy_str_data(path), "wb");
	if (!f)
	{
		io_fail("File.writeBytes: could not write file.");
		return;
	}

	int64_t n = bzy_array_len(data);
	int64_t *slots = (int64_t*)((char*)data + 32);
	for (int64_t i = 0; i < n; i++)
	{
		unsigned char b = (unsigned char)slots[i];
		fwrite(&b, 1, 1, f);
	}

	fclose(f);
}

/* Glob match supporting '*' (any run) and '?' (one char). Case-sensitive. */
static int glob_match(const char *p, const char *s)
{
	while (*p)
	{
		if (*p == '*')
		{
			while (*p == '*')
			{
				p++;
			}

			if (!*p)
			{
				return 1;
			}

			for (; *s; s++)
			{
				if (glob_match(p, s))
				{
					return 1;
				}
			}

			return glob_match(p, s);   /* s at end. */
		}

		if (*p == '?')
		{
			if (!*s)
			{
				return 0;
			}
		}
		else if (*p != *s)
		{
			return 0;
		}

		p++;
		s++;
	}

	return *s == '\0';
}

/* A growable list of owned bzy strings (full paths) collected during a walk. */
typedef struct
{
	void **items;
	int64_t count;
	int64_t cap;
} PathList;

static void pl_push(PathList *pl, void *s)
{
	if (pl->count == pl->cap)
	{
		pl->cap = pl->cap ? pl->cap * 2 : 16;
		pl->items = (void**)realloc(pl->items, (size_t)pl->cap * sizeof(void*));
	}

	pl->items[pl->count++] = s;
}

/* Enumerate folder; push full paths whose name matches pattern (NULL = all);
   recurse into subdirectories when recursive. */
static void walk_dir(const char *folder, const char *pattern, int recursive, PathList *pl)
{
#ifdef _WIN32
	char pat[1024];
	snprintf(pat, sizeof(pat), "%s\\*", folder);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		return;
	}

	do
	{
		const char *name = fd.cFileName;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		{
			continue;
		}

		char full[1024];
		snprintf(full, sizeof(full), "%s\\%s", folder, name);
		int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
		if (!pattern || glob_match(pattern, name))
		{
			pl_push(pl, bzy_str_new(full, (int64_t)strlen(full)));
		}

		if (recursive && isdir)
		{
			walk_dir(full, pattern, 1, pl);
		}
	}
	while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *dir = opendir(folder);
	if (!dir)
	{
		return;
	}

	struct dirent *e;
	while ((e = readdir(dir)) != NULL)
	{
		const char *name = e->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		{
			continue;
		}

		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", folder, name);
		struct stat st;
		int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
		if (!pattern || glob_match(pattern, name))
		{
			pl_push(pl, bzy_str_new(full, (int64_t)strlen(full)));
		}

		if (recursive && isdir)
		{
			walk_dir(full, pattern, 1, pl);
		}
	}

	closedir(dir);
#endif
}

/* Validate the folder, walk it, and hand the collected paths to an owned string[]. */
static void *build_search(void *folder, const char *pattern, int recursive, const char *who)
{
	const char *f = bzy_str_data(folder);
	int isdir = 0;
	if (!file_stat(f, &isdir) || !isdir)
	{
		io_fail(who);
		return NULL;
	}

	PathList pl = { NULL, 0, 0 };
	walk_dir(f, pattern, recursive, &pl);

	void *arr = bzy_array_new(pl.count, 1);
	void **elems = (void**)((char*)arr + 32);
	for (int64_t i = 0; i < pl.count; i++)
	{
		elems[i] = pl.items[i];   /* Owned; transferred to the array. */
	}

	free(pl.items);
	return arr;
}

void *bzy_file_list(void *folder)
{
	return build_search(folder, NULL, 0, "File.list: not a folder.");
}

void *bzy_file_search(void *folder, void *pattern)
{
	return build_search(folder, bzy_str_data(pattern), 0, "File.search: not a folder.");
}

void *bzy_file_search_recursive(void *folder, void *pattern)
{
	return build_search(folder, bzy_str_data(pattern), 1, "File.searchRecursive: not a folder.");
}

/* Attribute bits match the Windows FILE_ATTRIBUTE_* constants:
   READONLY=1, HIDDEN=2, SYSTEM=4, ARCHIVE=32. */
void bzy_file_set_attribute(void *path, int64_t attr, int64_t on)
{
	const char *p = bzy_str_data(path);
#ifdef _WIN32
	DWORD a = GetFileAttributesA(p);
	if (a == INVALID_FILE_ATTRIBUTES)
	{
		io_fail("File.setAttribute: path does not exist.");
		return;
	}

	if (on)
	{
		a |= (DWORD)attr;
	}
	else
	{
		a &= ~(DWORD)attr;
	}

	if (!SetFileAttributesA(p, a))
	{
		io_fail("File.setAttribute: could not set attributes.");
	}
#else
	struct stat st;
	if (stat(p, &st) != 0)
	{
		io_fail("File.setAttribute: path does not exist.");
		return;
	}

	if (attr & 1)   /* READONLY: the only portable attribute. */
	{
		mode_t m = on ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | 0200);
		if (chmod(p, m) != 0)
		{
			io_fail("File.setAttribute: could not set attributes.");
		}
	}
	/* HIDDEN/SYSTEM/ARCHIVE have no portable equivalent: no-op. */
#endif
}

int64_t bzy_file_has_attribute(void *path, int64_t attr)
{
	const char *p = bzy_str_data(path);
#ifdef _WIN32
	DWORD a = GetFileAttributesA(p);
	if (a == INVALID_FILE_ATTRIBUTES)
	{
		return 0;
	}

	return (a & (DWORD)attr) ? 1 : 0;
#else
	struct stat st;
	if (stat(p, &st) != 0)
	{
		return 0;
	}

	if (attr & 1)
	{
		return (st.st_mode & 0222) ? 0 : 1;   /* READONLY = no write bits. */
	}

	return 0;
#endif
}

/* Offload helpers: run a blocking file op on the pool while the breeze parks, then
   transfer the worker's io-error into this (breeze) thread so the codegen-emitted
   bzy_io_check sees it. Outside a breeze (bzy_sched_current()==NULL) run inline.
   The caller owns path/data/result across the park; the worker only reads them. */

typedef struct
{
	void *(*fn)(void*);
	void *a0;
	void *result;
	const char *err;
} Off1;
static void off1_run(void *p)
{
	Off1 *c = (Off1*)p;
	c->result = c->fn(c->a0);
	c->err = g_io_error;     /* Capture on the worker thread; */
	g_io_error = NULL;       /* leave the worker thread's flag clean. */
}
static void *offload1(void *(*fn)(void*), void *a0)
{
	if (!bzy_sched_current())
	{
		return fn(a0);                 /* No breeze: run inline. */
	}

	Off1 c = { fn, a0, NULL, NULL };
	bzy_offload_run(off1_run, &c);
	g_io_error = c.err;                /* Transfer onto the breeze thread for bzy_io_check. */
	return c.result;
}

typedef struct
{
	void (*fn)(void*,void*);
	void *a0;
	void *a1;
	const char *err;
} Off2v;
static void off2v_run(void *p)
{
	Off2v *c = (Off2v*)p;
	c->fn(c->a0, c->a1);
	c->err = g_io_error;
	g_io_error = NULL;
}
static void offload2v(void (*fn)(void*,void*), void *a0, void *a1)
{
	if (!bzy_sched_current())
	{
		fn(a0, a1);
		return;
	}

	Off2v c = { fn, a0, a1, NULL };
	bzy_offload_run(off2v_run, &c);
	g_io_error = c.err;
}

void *bzy_file_read_text(void *path)
{
	return offload1(real_read_text, path);
}
void *bzy_file_read_lines(void *path)
{
	return offload1(real_read_lines, path);
}
void *bzy_file_read_bytes(void *path)
{
	return offload1(real_read_bytes, path);
}
void  bzy_file_write_text(void *path, void *c)
{
	offload2v(real_write_text, path, c);
}
void  bzy_file_append_text(void *path, void *c)
{
	offload2v(real_append_text, path, c);
}
void  bzy_file_write_bytes(void *path, void *d)
{
	offload2v(real_write_bytes, path, d);
}
