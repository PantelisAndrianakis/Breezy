/* Memory-mapped file: a whole-file read-write map (MAP_SHARED / PAGE_READWRITE)
   exposing the file's bytes with no per-access syscall. Own TU: a program that
   never references FileChannel.map / MappedFile drops this object. mmap and
   CreateFileMapping are OS core (no dlopen). The map reuses the FileChannel's
   already-writable handle/fd; the raw pointer is process-global, so the owning
   breeze may migrate workers freely (the handle is shared cross-core). */
#include "breezy.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

/* FileChannel handle layout (see runtime/filechannel.c): +24 = HANDLE/fd, +32 = closed. */
#define FC_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

typedef struct { void *base; int64_t len; void *win_map; int64_t src; } MmapCtrl;

/* MappedFile handle layout (object_size = 40):
   0 vtable | 8 rc | 16 gcinfo | 24 ctrl(MmapCtrl*) | 32 closed(int64). */
#define MF_CTRL(o)   (*(MmapCtrl**)((char*)(o) + 24))
#define MF_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static int64_t g_mf_typeinfo[2] = { 0, 0 };
static int64_t g_mf_vtable[2];

static void mf_teardown(MmapCtrl *c)
{
#ifdef _WIN32
	if (c->base) { FlushViewOfFile(c->base, 0); UnmapViewOfFile(c->base); }
	if (c->win_map) { CloseHandle((HANDLE)c->win_map); }
#else
	if (c->base && c->base != MAP_FAILED) { munmap(c->base, (size_t)c->len); }
#endif
	free(c);
}

static void mf_finalize(void *o)
{
	if (MF_CLOSED(o)) { return; }
	if (MF_CTRL(o)) { mf_teardown(MF_CTRL(o)); MF_CTRL(o) = NULL; }
	MF_CLOSED(o) = 1;
}

static void *mf_vtable(void)
{
	g_mf_typeinfo[0] = (int64_t)(void*)mf_finalize;
	g_mf_vtable[0] = (int64_t)&g_mf_typeinfo[0];
	return &g_mf_vtable[1];
}

void *bzy_mmap_map(void *fc)
{
	if (!fc || FC_CLOSED(fc)) { bzy_io_fail("FileChannel.map: channel is closed."); return NULL; }

#ifdef _WIN32
	HANDLE h = *(HANDLE*)((char*)fc + 24);
	LARGE_INTEGER li;
	if (!GetFileSizeEx(h, &li)) { bzy_io_fail("FileChannel.map: could not size the file."); return NULL; }
	int64_t len = (int64_t)li.QuadPart;
	if (len <= 0) { bzy_io_fail("FileChannel.map: cannot map an empty file (truncate it first)."); return NULL; }
	HANDLE map = CreateFileMappingA(h, NULL, PAGE_READWRITE, li.HighPart, li.LowPart, NULL);
	if (!map) { bzy_io_fail("FileChannel.map: CreateFileMapping failed."); return NULL; }
	void *base = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, (SIZE_T)len);
	if (!base) { CloseHandle(map); bzy_io_fail("FileChannel.map: MapViewOfFile failed."); return NULL; }
	int64_t src = (int64_t)(intptr_t)h;
	void *win_map = (void*)map;
#else
	int fd = (int)*(int64_t*)((char*)fc + 24);
	struct stat stt;
	if (fstat(fd, &stt) != 0) { bzy_io_fail("FileChannel.map: could not size the file."); return NULL; }
	int64_t len = (int64_t)stt.st_size;
	if (len <= 0) { bzy_io_fail("FileChannel.map: cannot map an empty file (truncate it first)."); return NULL; }
	void *base = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) { bzy_io_fail("FileChannel.map: mmap failed."); return NULL; }
	int64_t src = (int64_t)fd;
	void *win_map = NULL;
#endif

	MmapCtrl *c = (MmapCtrl*)calloc(1, sizeof(MmapCtrl));
	if (!c)
	{
#ifdef _WIN32
		UnmapViewOfFile(base); CloseHandle((HANDLE)win_map);
#else
		munmap(base, (size_t)len);
#endif
		bzy_io_fail("FileChannel.map: out of memory."); return NULL;
	}
	c->base = base; c->len = len; c->win_map = win_map; c->src = src;

	void *o = bzy_alloc(40);
	*(void**)o = mf_vtable();
	MF_CTRL(o) = c;
	MF_CLOSED(o) = 0;
	bzy_share_crosscore(o);
	return o;
}

/* Anonymous memory region: a zero-filled, page-aligned MappedFile not backed by
   any file. Linux uses MAP_ANONYMOUS; Windows a page-file-backed file mapping
   (INVALID_HANDLE_VALUE), so the same MappedFile object, accessors, and teardown
   apply. src = -1 marks it anonymous (flush has nothing to sync to disk). */
void *bzy_memory_map(int64_t bytes)
{
	if (bytes <= 0) { bzy_io_fail("Memory.map: size must be positive."); return NULL; }

#ifdef _WIN32
	HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
	                                (DWORD)((uint64_t)bytes >> 32), (DWORD)((uint64_t)bytes & 0xffffffffu), NULL);
	if (!map) { bzy_io_fail("Memory.map: CreateFileMapping failed."); return NULL; }
	void *base = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, (SIZE_T)bytes);
	if (!base) { CloseHandle(map); bzy_io_fail("Memory.map: MapViewOfFile failed."); return NULL; }
	void *win_map = (void*)map;
#else
	void *base = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) { bzy_io_fail("Memory.map: mmap failed."); return NULL; }
	void *win_map = NULL;
#endif

	MmapCtrl *c = (MmapCtrl*)calloc(1, sizeof(MmapCtrl));
	if (!c)
	{
#ifdef _WIN32
		UnmapViewOfFile(base); CloseHandle((HANDLE)win_map);
#else
		munmap(base, (size_t)bytes);
#endif
		bzy_io_fail("Memory.map: out of memory."); return NULL;
	}
	c->base = base; c->len = bytes; c->win_map = win_map; c->src = -1;

	void *o = bzy_alloc(40);
	*(void**)o = mf_vtable();
	MF_CTRL(o) = c;
	MF_CLOSED(o) = 0;
	bzy_share_crosscore(o);
	return o;
}

int64_t bzy_mmap_size(void *m)
{
	if (MF_CLOSED(m)) { return 0; }
	return MF_CTRL(m)->len;
}

int64_t bzy_mmap_get_byte(void *m, int64_t i)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	if (i < 0 || i >= len) { bzy_oob_abort(i, len); }
	return (int64_t)*(uint8_t*)((char*)c->base + i);
}

int64_t bzy_mmap_get_int(void *m, int64_t i)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	if (i < 0 || i + 4 > len) { bzy_oob_abort(i, len); }
	int32_t v;
	memcpy(&v, (char*)c->base + i, 4);
	return (int64_t)v;
}

int64_t bzy_mmap_get_long(void *m, int64_t i)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	if (i < 0 || i + 8 > len) { bzy_oob_abort(i, len); }
	int64_t v;
	memcpy(&v, (char*)c->base + i, 8);
	return v;
}

void bzy_mmap_put_byte(void *m, int64_t i, int64_t v)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	if (i < 0 || i >= len) { bzy_oob_abort(i, len); }
	*(uint8_t*)((char*)c->base + i) = (uint8_t)v;
}

void bzy_mmap_put_int(void *m, int64_t i, int64_t v)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	if (i < 0 || i + 4 > len) { bzy_oob_abort(i, len); }
	int32_t t = (int32_t)v;
	memcpy((char*)c->base + i, &t, 4);
}

void bzy_mmap_put_long(void *m, int64_t i, int64_t v)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	if (i < 0 || i + 8 > len) { bzy_oob_abort(i, len); }
	memcpy((char*)c->base + i, &v, 8);
}

void bzy_mmap_copy_into(void *m, void *dst, int64_t srcOff, int64_t n)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	int64_t dstlen = bzy_array_len(dst);
	if (n < 0 || srcOff < 0 || srcOff + n > len || n > dstlen) { bzy_oob_abort(srcOff, len); }
	memcpy((char*)dst + 32, (char*)c->base + srcOff, (size_t)n);   /* Packed byte[] payload at +32. No syscall. */
}

/* The reverse of copy_into: bulk-write a byte[] into the region at dstOff. */
void bzy_mmap_copy_from(void *m, void *src, int64_t dstOff, int64_t n)
{
	MmapCtrl *c = MF_CTRL(m);
	int64_t len = MF_CLOSED(m) ? 0 : c->len;
	int64_t srclen = bzy_array_len(src);
	if (n < 0 || dstOff < 0 || dstOff + n > len || n > srclen) { bzy_oob_abort(dstOff, len); }
	memcpy((char*)c->base + dstOff, (char*)src + 32, (size_t)n);   /* Packed byte[] payload at +32. */
}

/* flush: offloaded (blocks), mirrors filechannel sync's result-code pattern. */
typedef struct { MmapCtrl *c; int err; } FlushCtx;

static void mmap_flush_run(void *p)
{
	FlushCtx *f = (FlushCtx*)p;
#ifdef _WIN32
	if (!FlushViewOfFile(f->c->base, 0)) { f->err = 1; return; }
	if (!FlushFileBuffers((HANDLE)(intptr_t)f->c->src)) { f->err = 1; }
#else
	if (msync(f->c->base, (size_t)f->c->len, MS_SYNC) != 0) { f->err = 1; }
#endif
}

void bzy_mmap_flush(void *m)
{
	if (MF_CLOSED(m)) { bzy_io_fail("MappedFile.flush: map is closed."); return; }
	if (MF_CTRL(m)->src < 0) { return; }   /* Anonymous region: nothing to sync to disk. */
	FlushCtx f = { MF_CTRL(m), 0 };
	bzy_offload_run(mmap_flush_run, &f);
	if (f.err) { bzy_io_fail("MappedFile.flush: could not flush to disk."); }
}

void bzy_mmap_close(void *m)
{
	if (MF_CLOSED(m)) { return; }
	mf_teardown(MF_CTRL(m));
	MF_CTRL(m) = NULL;
	MF_CLOSED(m) = 1;
}
