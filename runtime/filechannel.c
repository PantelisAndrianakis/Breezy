#include "breezy.h"
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/stat.h>
#endif

/* FileChannel managed leaf (object_size = 40):
   0 vtable | 8 rc | 16 gcinfo | 24 handle | 32 closed(int64).
   The handle slot is a Win32 HANDLE on Windows and an int fd on POSIX. */
#define FC_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static int64_t g_fc_typeinfo[2] = { 0 /* Finalizer (set on first use). */, 0 };
static int64_t g_fc_vtable[2];

#ifdef _WIN32
/* ---- Windows: positioned overlapped I/O on the process IOCP port. ---- */
#define FC_HANDLE(o) (*(HANDLE*)((char*)(o) + 24))

static void fc_finalize(void *o)
{
	if (!FC_CLOSED(o) && FC_HANDLE(o) != INVALID_HANDLE_VALUE)
	{
		CloseHandle(FC_HANDLE(o));
		FC_CLOSED(o) = 1;
	}
}

static void *fc_vtable(void)
{
	g_fc_typeinfo[0] = (int64_t)(void*)fc_finalize;
	g_fc_vtable[0] = (int64_t)&g_fc_typeinfo[0];
	return &g_fc_vtable[1];
}

void *bzy_filechannel_open(void *path)
{
	/* Synchronous handle (no FILE_FLAG_OVERLAPPED): a positioned ReadFile/WriteFile
	   then blocks and returns its byte count inline, so readAt/writeAt run as a bare
	   syscall on the calling worker with no IOCP round-trip. Overlapped file reads
	   queue an async IRP even on a cache hit, so the completion-port path paid a
	   thread round-trip per op; this is the Windows analogue of the inline pread the
	   POSIX path uses. A cold op briefly blocks the worker - the work-stealing
	   scheduler drains its other ready breezes. sync stays offloaded (it blocks). */
	HANDLE h = CreateFileA(bzy_str_data(path), GENERIC_READ | GENERIC_WRITE,
						   FILE_SHARE_READ, NULL, OPEN_ALWAYS,
						   FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		bzy_io_fail("File.openChannel: could not open file.");
		return NULL;   /* The codegen-emitted bzy_io_check throws before this is used. */
	}

	void *o = bzy_alloc(40);
	*(void**)o = fc_vtable();
	FC_HANDLE(o) = h;
	FC_CLOSED(o) = 0;
	return o;
}

void *bzy_filechannel_read_at(void *ch, int64_t offset, int64_t maxbytes)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.readAt: channel is closed.");
		return bzy_array_new_sized(0, 1, 0);   /* Empty packed byte[]. */
	}

	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	/* Packed byte[]: ReadFile fills the element region in place, so there is no
	   intermediate buffer and no copy (the slotted layout used to require both). */
	void *arr = bzy_array_new_sized(maxbytes, 1, 0);

	OVERLAPPED ov = {0};             /* Carries the offset only; the handle is synchronous. */
	ov.Offset = (DWORD)(offset & 0xffffffff);
	ov.OffsetHigh = (DWORD)((offset >> 32) & 0xffffffff);

	DWORD got = 0;
	BOOL ok = ReadFile(FC_HANDLE(ch), (char*)arr + 32, (DWORD)maxbytes, &got, &ov);   /* Blocks, returns inline. */
	if (!ok && GetLastError() != ERROR_HANDLE_EOF)
	{
		bzy_io_fail("FileChannel.readAt: read failed.");
		got = 0;
	}

	if ((int64_t)got != maxbytes)
	{
		*(int64_t*)((char*)arr + 24) = (int64_t)got;   /* Logical length = bytes actually read. */
	}

	return arr;
}

int64_t bzy_filechannel_write_at(void *ch, int64_t offset, void *data)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.writeAt: channel is closed.");
		return 0;
	}

	int64_t total = bzy_array_len(data);
	const char *buf = (const char*)data + 32;   /* Packed bytes, no copy. */

	int64_t off = 0;
	while (off < total)
	{
		OVERLAPPED ov = {0};         /* Carries the offset only; the handle is synchronous. */
		int64_t at = offset + off;
		ov.Offset = (DWORD)(at & 0xffffffff);
		ov.OffsetHigh = (DWORD)((at >> 32) & 0xffffffff);

		DWORD got = 0;
		BOOL ok = WriteFile(FC_HANDLE(ch), buf + off, (DWORD)(total - off), &got, &ov);   /* Blocks. */
		if (!ok)
		{
			bzy_io_fail("FileChannel.writeAt: write failed.");
			break;
		}

		if (got == 0)
		{
			break;
		}

		off += (int64_t)got;
	}

	return off;
}

int64_t bzy_filechannel_size(void *ch)
{
	if (FC_CLOSED(ch))
	{
		return -1;
	}

	LARGE_INTEGER li;
	if (!GetFileSizeEx(FC_HANDLE(ch), &li))
	{
		return -1;
	}

	return (int64_t)li.QuadPart;
}

void bzy_filechannel_truncate(void *ch, int64_t size)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.truncate: channel is closed.");
		return;
	}

	LARGE_INTEGER li;
	li.QuadPart = size;
	if (!SetFilePointerEx(FC_HANDLE(ch), li, NULL, FILE_BEGIN) || !SetEndOfFile(FC_HANDLE(ch)))
	{
		bzy_io_fail("FileChannel.truncate: could not set file length.");
	}
}

/* FlushFileBuffers has no overlapped form; run it on the offload pool so the breeze
   parks. The worker stores success in the ctx; the breeze thread reports any error
   (no g_io_error crosses a thread). */
typedef struct
{
	HANDLE h;
	int ok;
} SyncCtx;
static void sync_run(void *p)
{
	SyncCtx *c = (SyncCtx*)p;
	c->ok = FlushFileBuffers(c->h) ? 1 : 0;
}

void bzy_filechannel_sync(void *ch)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.sync: channel is closed.");
		return;
	}

	if (bzy_sched_current())
	{
		SyncCtx c = { FC_HANDLE(ch), 0 };
		bzy_offload_run(sync_run, &c);
		if (!c.ok)
		{
			bzy_io_fail("FileChannel.sync: flush failed.");
		}
	}
	else
	{
		if (!FlushFileBuffers(FC_HANDLE(ch)))
		{
			bzy_io_fail("FileChannel.sync: flush failed.");
		}
	}
}

void bzy_filechannel_close(void *ch)
{
	if (!FC_CLOSED(ch) && FC_HANDLE(ch) != INVALID_HANDLE_VALUE)
	{
		CloseHandle(FC_HANDLE(ch));
		FC_CLOSED(ch) = 1;
	}
}

#else
/* ---- POSIX: positioned pread/pwrite run on the offload pool so the breeze
   parks (no completion port). The handle slot holds an int fd. ---- */
#define FC_FD(o) (*(int64_t*)((char*)(o) + 24))

static void fc_finalize(void *o)
{
	if (!FC_CLOSED(o) && FC_FD(o) >= 0)
	{
		close((int)FC_FD(o));
		FC_CLOSED(o) = 1;
	}
}

static void *fc_vtable(void)
{
	g_fc_typeinfo[0] = (int64_t)(void*)fc_finalize;
	g_fc_vtable[0] = (int64_t)&g_fc_typeinfo[0];
	return &g_fc_vtable[1];
}

void *bzy_filechannel_open(void *path)
{
	int fd = open(bzy_str_data(path), O_RDWR | O_CREAT, 0644);   /* OPEN_ALWAYS: create if absent, keep contents. */
	if (fd < 0)
	{
		bzy_io_fail("File.openChannel: could not open file.");
		return NULL;   /* The codegen-emitted bzy_io_check throws before this is used. */
	}

	void *o = bzy_alloc(40);
	*(void**)o = fc_vtable();
	FC_FD(o) = fd;
	FC_CLOSED(o) = 0;
	return o;
}

/* Offloaded positioned read: pread is atomic w.r.t. the fd offset. */
typedef struct
{
	int fd;
	char *buf;
	int64_t off;
	int64_t max;
	int n;          /* Bytes read (0 = EOF). */
	int err;
} RdCtx;
static void rd_run(void *p)
{
	RdCtx *c = (RdCtx*)p;
	ssize_t r = pread(c->fd, c->buf, (size_t)c->max, (off_t)c->off);
	if (r < 0)
	{
		c->err = 1;
		c->n = 0;
	}
	else
	{
		c->n = (int)r;
	}
}

void *bzy_filechannel_read_at(void *ch, int64_t offset, int64_t maxbytes)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.readAt: channel is closed.");
		return bzy_array_new_sized(0, 1, 0);   /* Empty packed byte[]. */
	}

	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	/* Packed byte[]: pread fills the element region in place, so there is no
	   intermediate buffer and no copy (the slotted layout used to require both). */
	void *arr = bzy_array_new_sized(maxbytes, 1, 0);
	RdCtx c = { (int)FC_FD(ch), (char*)arr + 32, offset, maxbytes, 0, 0 };
	/* Run the pread inline on the calling worker instead of handing off to the
	   offload pool. A positioned read is usually a cache hit that returns in well
	   under a microsecond, so the two thread hops the pool costs dominate it; if a
	   cold read does briefly block this worker, the work-stealing scheduler drains
	   its other ready breezes. (sync stays offloaded - fsync genuinely blocks.) */
	rd_run(&c);

	if (c.err)
	{
		bzy_io_fail("FileChannel.readAt: read failed.");
		c.n = 0;
	}

	if ((int64_t)c.n != maxbytes)
	{
		*(int64_t*)((char*)arr + 24) = (int64_t)c.n;   /* Logical length = bytes actually read. */
	}

	return arr;
}

/* Offloaded positioned write: a pwrite loop until the buffer drains. */
typedef struct
{
	int fd;
	const char *buf;
	int64_t off;
	int64_t total;
	int64_t written;
	int err;
} WrCtx;
static void wr_run(void *p)
{
	WrCtx *c = (WrCtx*)p;
	int64_t o = 0;
	while (o < c->total)
	{
		ssize_t w = pwrite(c->fd, c->buf + o, (size_t)(c->total - o), (off_t)(c->off + o));
		if (w <= 0)
		{
			c->err = 1;
			break;
		}

		o += (int64_t)w;
	}

	c->written = o;
}

int64_t bzy_filechannel_write_at(void *ch, int64_t offset, void *data)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.writeAt: channel is closed.");
		return 0;
	}

	int64_t total = bzy_array_len(data);
	const char *buf = (const char*)data + 32;   /* Packed bytes, no copy. */

	WrCtx c = { (int)FC_FD(ch), buf, offset, total, 0, 0 };
	/* Inline pwrite on the calling worker (see readAt): the offload pool's two
	   thread hops dwarf a cached positioned write. sync stays offloaded. */
	wr_run(&c);

	if (c.err)
	{
		bzy_io_fail("FileChannel.writeAt: write failed.");
	}

	return c.written;
}

int64_t bzy_filechannel_size(void *ch)
{
	if (FC_CLOSED(ch))
	{
		return -1;
	}

	struct stat st;
	if (fstat((int)FC_FD(ch), &st) != 0)
	{
		return -1;
	}

	return (int64_t)st.st_size;
}

void bzy_filechannel_truncate(void *ch, int64_t size)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.truncate: channel is closed.");
		return;
	}

	if (ftruncate((int)FC_FD(ch), (off_t)size) != 0)
	{
		bzy_io_fail("FileChannel.truncate: could not set file length.");
	}
}

/* fsync run on the offload pool so the breeze parks. */
typedef struct
{
	int fd;
	int ok;
} SyncCtx;
static void sync_run(void *p)
{
	SyncCtx *c = (SyncCtx*)p;
	c->ok = (fsync(c->fd) == 0) ? 1 : 0;
}

void bzy_filechannel_sync(void *ch)
{
	if (FC_CLOSED(ch))
	{
		bzy_io_fail("FileChannel.sync: channel is closed.");
		return;
	}

	if (bzy_sched_current())
	{
		SyncCtx c = { (int)FC_FD(ch), 0 };
		bzy_offload_run(sync_run, &c);
		if (!c.ok)
		{
			bzy_io_fail("FileChannel.sync: flush failed.");
		}
	}
	else
	{
		if (fsync((int)FC_FD(ch)) != 0)
		{
			bzy_io_fail("FileChannel.sync: flush failed.");
		}
	}
}

void bzy_filechannel_close(void *ch)
{
	if (!FC_CLOSED(ch) && FC_FD(ch) >= 0)
	{
		close((int)FC_FD(ch));
		FC_CLOSED(ch) = 1;
	}
}

#endif
