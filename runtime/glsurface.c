/* OpenGL surface over SDL2, loaded dynamically at run time (no link-time dep).
   Own TU: a program that never references Graphics.openGL / GlSurface drops this
   object; even when present it needs no SDL2 to link (SDL2 is dlopen'd lazily, so
   absence degrades to a clean IOException).

   Threading (differs from runtime/surface.c -- there is NO render thread): a GL
   context becomes current on the thread that creates it, so the CALLING BREEZE's
   worker creates the window+context and the breeze itself runs the whole
   poll/draw/swap loop on that worker. A running breeze is never stolen, so the
   loop stays pinned as long as it never awaits; swapBuffers() stamps and checks
   the owning OS-thread id and aborts clearly if the breeze migrated mid-frame. */
#include "breezy.h"
#include "platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#ifndef _WIN32
  #include <dlfcn.h>
  #include <pthread.h>
#endif

#ifdef _WIN32
  #undef CreateWindow   /* windows.h macro-expands CreateWindow -> CreateWindowA. */
#endif

/* ---- dl helpers (mirror runtime/surface.c). ---- */
#ifdef _WIN32
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		HMODULE h = LoadLibraryA(names[i]);
		if (h) { return (void*)h; }
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n) { return (void*)GetProcAddress((HMODULE)h, n); }
#else
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
		if (h) { return h; }
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n) { return dlsym(h, n); }
#endif

/* ---- SDL2 + GL constants (ABI-stable in SDL2). ---- */
#define BZ_SDL_INIT_VIDEO          0x00000020u
#define BZ_SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
#define BZ_SDL_WINDOW_OPENGL       0x00000002u
#define BZ_SDL_WINDOW_SHOWN        0x00000004u
#define BZ_SDL_GL_DOUBLEBUFFER     5            /* SDL_GLattr enum value. */
/* Event types (same packing as surface.c). */
#define BZ_SDL_QUIT             0x100u
#define BZ_SDL_WINDOWEVENT      0x200u
#define BZ_SDL_KEYDOWN          0x300u
#define BZ_SDL_KEYUP            0x301u
#define BZ_SDL_MOUSEMOTION      0x400u
#define BZ_SDL_MOUSEBUTTONDOWN  0x401u
#define BZ_SDL_WINDOWEVENT_CLOSE 14

static struct
{
	int loaded;            /* 0 = not tried, 1 = ok, -1 = failed. */
	int  (*Init)(uint32_t flags);
	int  (*GL_SetAttribute)(int attr, int value);
	void *(*CreateWindow)(const char *title, int x, int y, int w, int h, uint32_t flags);
	void (*DestroyWindow)(void *win);
	void *(*GL_CreateContext)(void *win);
	void (*GL_DeleteContext)(void *ctx);
	int  (*GL_MakeCurrent)(void *win, void *ctx);
	void (*GL_SwapWindow)(void *win);
	void *(*GL_GetProcAddress)(const char *name);
	int  (*PollEvent)(void *event);
} sdl;

static int gl_load(void)
{
	if (sdl.loaded == 1) { return 1; }
	if (sdl.loaded == -1) { bzy_io_fail("OpenGL surface unavailable: SDL2 not found."); return 0; }

#ifdef _WIN32
	const char *names[] = { "SDL2.dll", NULL };
#else
	const char *names[] = { "libSDL2-2.0.so.0", "libSDL2.so", NULL };
#endif
	void *h = dl_open_first(names);
	if (!h) { sdl.loaded = -1; bzy_io_fail("OpenGL surface unavailable: SDL2 not found."); return 0; }

	#define SYM(field, name) do { \
		*(void**)(&sdl.field) = dl_sym(h, name); \
		if (!sdl.field) { sdl.loaded = -1; bzy_io_fail("OpenGL surface unavailable: SDL2 symbol missing."); return 0; } \
	} while (0)
	SYM(Init, "SDL_Init");
	SYM(GL_SetAttribute, "SDL_GL_SetAttribute");
	SYM(CreateWindow, "SDL_CreateWindow");
	SYM(DestroyWindow, "SDL_DestroyWindow");
	SYM(GL_CreateContext, "SDL_GL_CreateContext");
	SYM(GL_DeleteContext, "SDL_GL_DeleteContext");
	SYM(GL_MakeCurrent, "SDL_GL_MakeCurrent");
	SYM(GL_SwapWindow, "SDL_GL_SwapWindow");
	SYM(GL_GetProcAddress, "SDL_GL_GetProcAddress");
	SYM(PollEvent, "SDL_PollEvent");
	#undef SYM

	sdl.loaded = 1;
	return 1;
}

/* The resolver bzy_dynsym calls for `extern dynamic` GL symbols (live context). */
static void *gl_getproc(const char *name)
{
	return sdl.GL_GetProcAddress ? sdl.GL_GetProcAddress(name) : NULL;
}

/* ---- Native control struct (C heap, not a Breezy object). ---- */
typedef struct
{
	void *win, *ctx;
	uintptr_t owner_tid;   /* OS thread that holds the context current. */
} GlCtrl;

/* ---- GlSurface handle layout (object_size = 40):
   0 vtable | 8 rc | 16 gcinfo | 24 ctrl(GlCtrl*) | 32 closed(int64). ---- */
#define GL_CTRL(o)   (*(GlCtrl**)((char*)(o) + 24))
#define GL_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static int64_t g_gl_typeinfo[2] = { 0, 0 };
static int64_t g_gl_vtable[2];

static uintptr_t cur_tid(void)
{
#ifdef _WIN32
	return (uintptr_t)GetCurrentThreadId();
#else
	return (uintptr_t)pthread_self();
#endif
}

static void gl_teardown(GlCtrl *c)
{
	if (c->ctx) { sdl.GL_DeleteContext(c->ctx); }
	if (c->win) { sdl.DestroyWindow(c->win); }
	free(c);
}

static void gl_finalize(void *o)
{
	if (GL_CLOSED(o)) { return; }
	if (GL_CTRL(o)) { gl_teardown(GL_CTRL(o)); GL_CTRL(o) = NULL; }
	GL_CLOSED(o) = 1;
}

static void *gl_vtable(void)
{
	g_gl_typeinfo[0] = (int64_t)(void*)gl_finalize;
	g_gl_vtable[0] = (int64_t)&g_gl_typeinfo[0];
	return &g_gl_vtable[1];
}

void *bzy_glsurface_open(int64_t w, int64_t h, void *title)
{
	if (!gl_load()) { return NULL; }   /* gl_load already called bzy_io_fail. */
	if (w <= 0 || h <= 0) { bzy_io_fail("Graphics.openGL: width and height must be positive."); return NULL; }

	if (sdl.Init(BZ_SDL_INIT_VIDEO) != 0)
	{
		bzy_io_fail("Graphics.openGL: SDL video init failed (no display?)."); return NULL;
	}
	sdl.GL_SetAttribute(BZ_SDL_GL_DOUBLEBUFFER, 1);   /* Default/compat context: immediate mode stays available. */

	const char *tt = title ? bzy_str_data(title) : "";
	void *win = sdl.CreateWindow(tt, BZ_SDL_WINDOWPOS_UNDEFINED, BZ_SDL_WINDOWPOS_UNDEFINED,
	                             (int)w, (int)h, BZ_SDL_WINDOW_OPENGL | BZ_SDL_WINDOW_SHOWN);
	if (!win) { bzy_io_fail("Graphics.openGL: window creation failed."); return NULL; }
	void *ctx = sdl.GL_CreateContext(win);
	if (!ctx) { sdl.DestroyWindow(win); bzy_io_fail("Graphics.openGL: GL context creation failed."); return NULL; }
	sdl.GL_MakeCurrent(win, ctx);   /* Current on THIS worker thread (synchronous, no offload). */

	bzy_dyn_set_resolver(gl_getproc);   /* extern dynamic gl* now resolve through the live context. */

	GlCtrl *c = (GlCtrl*)calloc(1, sizeof(GlCtrl));
	if (!c)
	{
		sdl.GL_DeleteContext(ctx); sdl.DestroyWindow(win);
		bzy_io_fail("Graphics.openGL: out of memory."); return NULL;
	}
	c->win = win;
	c->ctx = ctx;
	c->owner_tid = cur_tid();

	void *o = bzy_alloc(40);
	*(void**)o = gl_vtable();
	GL_CTRL(o) = c;
	GL_CLOSED(o) = 0;
	bzy_share_crosscore(o);   /* Handle may be touched after a (disallowed-but-survivable) migration; share to be safe. */
	return o;
}

int64_t bzy_glsurface_poll(void *s)
{
	if (GL_CLOSED(s)) { return 0; }
	char ev[64];   /* >= sizeof(SDL_Event) (56). */
	while (sdl.PollEvent(ev))
	{
		uint32_t t = *(uint32_t*)(ev + 0);
		if (t == BZ_SDL_QUIT
			|| (t == BZ_SDL_WINDOWEVENT && *(uint8_t*)(ev + 12) == BZ_SDL_WINDOWEVENT_CLOSE))
		{
			return (6LL << 48);
		}
		else if (t == BZ_SDL_KEYDOWN)
		{
			int32_t sym = *(int32_t*)(ev + 20);
			return (1LL << 48) | ((int64_t)(sym & 0xFFFFFF) << 24);
		}
		else if (t == BZ_SDL_KEYUP)
		{
			int32_t sym = *(int32_t*)(ev + 20);
			return (2LL << 48) | ((int64_t)(sym & 0xFFFFFF) << 24);
		}
		else if (t == BZ_SDL_MOUSEMOTION)
		{
			int32_t x = *(int32_t*)(ev + 20), y = *(int32_t*)(ev + 24);
			return (3LL << 48) | ((int64_t)(x & 0xFFFFFF) << 24) | (y & 0xFFFFFF);
		}
		else if (t == BZ_SDL_MOUSEBUTTONDOWN)
		{
			uint8_t b = *(uint8_t*)(ev + 16);
			int32_t x = *(int32_t*)(ev + 20);
			return (4LL << 48) | ((int64_t)(x & 0xFFFFFF) << 24) | b;
		}
		/* Else: unmapped event, keep draining. */
	}
	return 0;
}

void bzy_glsurface_swap(void *s)
{
	if (GL_CLOSED(s)) { bzy_io_fail("GlSurface.swapBuffers: surface is closed."); return; }
	GlCtrl *c = GL_CTRL(s);
	if (cur_tid() != c->owner_tid)
	{
		fprintf(stderr, "GlSurface.swapBuffers: GL context used off its owning thread; "
		                "do not await inside a GL render loop.\n");
		abort();
	}
	sdl.GL_SwapWindow(c->win);
}

int64_t bzy_glsurface_isopen(void *s)
{
	if (GL_CLOSED(s)) { return 0; }
	return 1;   /* Open until close(); SDL_QUIT is delivered via pollEvent (kind 6). */
}

void bzy_glsurface_close(void *s)
{
	if (GL_CLOSED(s)) { return; }
	gl_teardown(GL_CTRL(s));
	GL_CTRL(s) = NULL;
	GL_CLOSED(s) = 1;
}
