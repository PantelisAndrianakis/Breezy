#include "breezy.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* FileChannel managed leaf (object_size = 40):
   0 vtable | 8 rc | 16 gcinfo | 24 HANDLE | 32 closed(int64). The finalizer
   CloseHandle()s an unclosed handle. */
#define FC_HANDLE(o) (*(HANDLE*)((char*)(o) + 24))
#define FC_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static void fc_finalize(void *o)
{
	if (!FC_CLOSED(o) && FC_HANDLE(o) != INVALID_HANDLE_VALUE)
	{
		CloseHandle(FC_HANDLE(o));
		FC_CLOSED(o) = 1;
	}
}

static int64_t g_fc_typeinfo[2] = { 0 /* Finalizer (set on first use). */, 0 };
static int64_t g_fc_vtable[2];

static void *fc_vtable(void)
{
	g_fc_typeinfo[0] = (int64_t)(void*)fc_finalize;
	g_fc_vtable[0] = (int64_t)&g_fc_typeinfo[0];
	return &g_fc_vtable[1];
}

/* Build a value byte[] from buf[0..n) (one byte per 8-byte slot). */
static void *fc_to_array(const char *buf, int n)
{
	void *arr = bzy_array_new(n < 0 ? 0 : n, 0);
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int i = 0; i < n; i++)
	{
		slots[i] = (unsigned char)buf[i];
	}

	return arr;
}

void *bzy_filechannel_open(void *path)
{
	bzy_iocp_ensure();
	HANDLE h = CreateFileA(bzy_str_data(path), GENERIC_READ | GENERIC_WRITE,
						   FILE_SHARE_READ, NULL, OPEN_ALWAYS,
						   FILE_FLAG_OVERLAPPED, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		bzy_io_fail("File.openChannel: could not open file.");
		return NULL;   /* The codegen-emitted bzy_io_check throws before this is used. */
	}

	bzy_iocp_associate((void*)h);
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
		return fc_to_array(NULL, 0);
	}

	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = (char*)malloc((size_t)maxbytes);

	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);
	OVERLAPPED *ov = (OVERLAPPED*)bzy_iocp_op_overlapped(op);
	ov->Offset = (DWORD)(offset & 0xffffffff);
	ov->OffsetHigh = (DWORD)((offset >> 32) & 0xffffffff);

	int n;
	BOOL ok = ReadFile(FC_HANDLE(ch), buf, (DWORD)maxbytes, NULL, ov);
	if (!ok && GetLastError() == ERROR_HANDLE_EOF)
	{
		n = 0;                       /* Synchronous EOF: no completion posted; do not park. */
	}
	else if (!ok && GetLastError() != ERROR_IO_PENDING)
	{
		free(buf);
		bzy_io_fail("FileChannel.readAt: read failed.");
		return fc_to_array(NULL, 0);
	}
	else
	{
		bzy_iocp_park(op);           /* Completion (posted even on synchronous success). */
		int e = bzy_iocp_op_err(op);
		if (e && e != ERROR_HANDLE_EOF)
		{
			free(buf);
			bzy_io_fail("FileChannel.readAt: read failed.");
			return fc_to_array(NULL, 0);
		}

		n = (int)bzy_iocp_op_bytes(op);   /* 0 at/past EOF. */
	}

	void *arr = fc_to_array(buf, n);
	free(buf);
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
	int64_t *slots = (int64_t*)((char*)data + 32);
	char *buf = (char*)malloc((size_t)(total > 0 ? total : 1));
	for (int64_t i = 0; i < total; i++)
	{
		buf[i] = (char)(unsigned char)slots[i];
	}

	int64_t off = 0;
	while (off < total)
	{
		char opbuf[BZY_IOCP_OP_SIZE];
		IocpOp *op = (IocpOp*)opbuf;
		bzy_iocp_op_reset(op);
		OVERLAPPED *ov = (OVERLAPPED*)bzy_iocp_op_overlapped(op);
		int64_t at = offset + off;
		ov->Offset = (DWORD)(at & 0xffffffff);
		ov->OffsetHigh = (DWORD)((at >> 32) & 0xffffffff);

		BOOL ok = WriteFile(FC_HANDLE(ch), buf + off, (DWORD)(total - off), NULL, ov);
		if (!ok && GetLastError() != ERROR_IO_PENDING)
		{
			bzy_io_fail("FileChannel.writeAt: write failed.");
			break;
		}

		bzy_iocp_park(op);
		if (bzy_iocp_op_err(op))
		{
			bzy_io_fail("FileChannel.writeAt: write failed.");
			break;
		}

		DWORD w = bzy_iocp_op_bytes(op);
		if (w == 0)
		{
			break;
		}

		off += (int64_t)w;
	}

	free(buf);
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
