/* Software pixel surface over SDL2, loaded dynamically at run time (no link-time
   dependency). Own TU: a program that never references Graphics/Surface pulls this
   object only if its prelude names bzy_surface_*, and even then needs no SDL2 to
   link -- SDL2 is dlopen'd lazily, so absence degrades to a clean IOException.

   Threading: SDL video init, the window, the event pump, and rendering all run on
   one dedicated render thread (mirrors runtime/desktop.c's GTK-thread model). The
   owning breeze never calls SDL -- present() hands a framebuffer over a mutexed
   double-buffer, pollEvent() drains a single-producer/single-consumer event ring --
   so the breeze may migrate across scheduler workers freely. */
#include "breezy.h"
#include "platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef _WIN32
#undef CreateWindow   /* windows.h macro-expands CreateWindow -> CreateWindowA; we use it as a struct field + SDL symbol (resolved by string name). */
#endif

/* ---- dl helpers (mirror runtime/tls.c). ---- */
#ifdef _WIN32
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		HMODULE h = LoadLibraryA(names[i]);
		if (h)
		{
			return (void*)h;
		}
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n)
{
	return (void*)GetProcAddress((HMODULE)h, n);
}
#else
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
		if (h)
		{
			return h;
		}
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n)
{
	return dlsym(h, n);
}
#endif

/* ---- SDL2 constants (ABI-stable in SDL2). ---- */
#define BZ_SDL_INIT_VIDEO           0x00000020u
#define BZ_SDL_WINDOWPOS_UNDEFINED  0x1FFF0000
#define BZ_SDL_WINDOW_SHOWN         0x00000004u
#define BZ_SDL_RENDERER_ACCELERATED 0x00000002u   /* No PRESENTVSYNC: vsync stays off. */
#define BZ_SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define BZ_SDL_TEXTUREACCESS_STREAMING 1
/* Event types. */
#define BZ_SDL_QUIT             0x100u
#define BZ_SDL_WINDOWEVENT      0x200u
#define BZ_SDL_KEYDOWN          0x300u
#define BZ_SDL_KEYUP            0x301u
#define BZ_SDL_MOUSEMOTION      0x400u
#define BZ_SDL_MOUSEBUTTONDOWN  0x401u
#define BZ_SDL_WINDOWEVENT_CLOSE 14

/* Dynamically-resolved SDL2 entry points. */
static struct
{
	int loaded;            /* 0 = not tried, 1 = ok, -1 = failed. */
	int  (*Init)(uint32_t flags);
	void (*Quit)(void);
	void *(*CreateWindow)(const char *title, int x, int y, int w, int h, uint32_t flags);
	void (*DestroyWindow)(void *win);
	void *(*CreateRenderer)(void *win, int index, uint32_t flags);
	void (*DestroyRenderer)(void *ren);
	void *(*CreateTexture)(void *ren, uint32_t fmt, int access, int w, int h);
	void (*DestroyTexture)(void *tex);
	int  (*UpdateTexture)(void *tex, const void *rect, const void *pixels, int pitch);
	int  (*RenderClear)(void *ren);
	int  (*RenderCopy)(void *ren, void *tex, const void *src, const void *dst);
	void (*RenderPresent)(void *ren);
	int  (*PollEvent)(void *event);
} sdl;

static int surface_load(void)
{
	if (sdl.loaded == 1)
	{
		return 1;
	}
	if (sdl.loaded == -1)
	{
		bzy_io_fail("Surface unavailable: SDL2 not found.");
		return 0;
	}

#ifdef _WIN32
	const char *names[] = { "SDL2.dll", NULL };
#else
	const char *names[] = { "libSDL2-2.0.so.0", "libSDL2.so", NULL };
#endif
	void *h = dl_open_first(names);
	if (!h)
	{
		sdl.loaded = -1;
		bzy_io_fail("Surface unavailable: SDL2 not found.");
		return 0;
	}

#define SYM(field, name) do { \
		*(void**)(&sdl.field) = dl_sym(h, name); \
		if (!sdl.field) { sdl.loaded = -1; bzy_io_fail("Surface unavailable: SDL2 symbol missing."); return 0; } \
	} while (0)
	SYM(Init, "SDL_Init");
	SYM(Quit, "SDL_Quit");
	SYM(CreateWindow, "SDL_CreateWindow");
	SYM(DestroyWindow, "SDL_DestroyWindow");
	SYM(CreateRenderer, "SDL_CreateRenderer");
	SYM(DestroyRenderer, "SDL_DestroyRenderer");
	SYM(CreateTexture, "SDL_CreateTexture");
	SYM(DestroyTexture, "SDL_DestroyTexture");
	SYM(UpdateTexture, "SDL_UpdateTexture");
	SYM(RenderClear, "SDL_RenderClear");
	SYM(RenderCopy, "SDL_RenderCopy");
	SYM(RenderPresent, "SDL_RenderPresent");
	SYM(PollEvent, "SDL_PollEvent");
#undef SYM

	sdl.loaded = 1;
	return 1;
}

/* ---- Render-thread control block (a C heap struct, not a Breezy object). ---- */
typedef struct
{
	void *win, *ren, *tex;
	int w, h;
	char title[256];
	bzy_thread thread;
	int started;
	bzy_mutex lock;          /* Guards the double-buffer swap + frame_ready. */
	bzy_sem open_sem;        /* Posted once the window is ready or failed. */
	bzy_sem frame_sem;       /* Posted by present() to wake the render thread. */
	uint32_t *front, *back;  /* Double-buffered ARGB framebuffers (w*h each). */
	volatile int frame_ready, stop, open, init_failed;
	volatile long ring_head, ring_tail;   /* SPSC event ring. */
	int64_t ring[256];
} SurfaceCtrl;

/* ---- Surface handle layout (object_size = 40):
   0 vtable | 8 rc | 16 gcinfo | 24 ctrl(SurfaceCtrl*) | 32 closed(int64). Leaf:
   the finalizer tears the render thread down and frees the ctrl. ---- */
#define SURF_CTRL(o)   (*(SurfaceCtrl**)((char*)(o) + 24))
#define SURF_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static int64_t g_surf_typeinfo[2] = { 0, 0 };
static int64_t g_surf_vtable[2];

/* Push one packed event (render thread = sole producer; drop when full). */
static void ring_push(SurfaceCtrl *c, int64_t v)
{
	if (c->ring_head - c->ring_tail >= 256)
	{
		return;
	}
	c->ring[c->ring_head & 255] = v;
	c->ring_head++;
}

/* The dedicated render thread: owns ALL SDL state for this surface. */
static void *surface_thread(void *p)
{
	SurfaceCtrl *c = (SurfaceCtrl*)p;

	if (sdl.Init(BZ_SDL_INIT_VIDEO) != 0)
	{
		c->init_failed = 1;
		bzy_sem_post(&c->open_sem, 1);
		return NULL;
	}
	c->win = sdl.CreateWindow(c->title, BZ_SDL_WINDOWPOS_UNDEFINED, BZ_SDL_WINDOWPOS_UNDEFINED,
							  c->w, c->h, BZ_SDL_WINDOW_SHOWN);
	if (!c->win)
	{
		c->init_failed = 1;
		sdl.Quit();
		bzy_sem_post(&c->open_sem, 1);
		return NULL;
	}
	c->ren = sdl.CreateRenderer(c->win, -1, BZ_SDL_RENDERER_ACCELERATED);
	if (!c->ren)
	{
		sdl.DestroyWindow(c->win);
		c->init_failed = 1;
		sdl.Quit();
		bzy_sem_post(&c->open_sem, 1);
		return NULL;
	}
	c->tex = sdl.CreateTexture(c->ren, BZ_SDL_PIXELFORMAT_ARGB8888, BZ_SDL_TEXTUREACCESS_STREAMING, c->w, c->h);
	if (!c->tex)
	{
		sdl.DestroyRenderer(c->ren);
		sdl.DestroyWindow(c->win);
		c->init_failed = 1;
		sdl.Quit();
		bzy_sem_post(&c->open_sem, 1);
		return NULL;
	}

	c->open = 1;
	bzy_sem_post(&c->open_sem, 1);   /* Handshake: window ready. */

	char ev[64];   /* >= sizeof(SDL_Event) (56). */
	while (!c->stop)
	{
		while (sdl.PollEvent(ev))
		{
			uint32_t t = *(uint32_t*)(ev + 0);
			if (t == BZ_SDL_QUIT
					|| (t == BZ_SDL_WINDOWEVENT && *(uint8_t*)(ev + 12) == BZ_SDL_WINDOWEVENT_CLOSE))
			{
				c->open = 0;
				ring_push(c, (6LL << 48));
			}
			else if (t == BZ_SDL_KEYDOWN)
			{
				int32_t sym = *(int32_t*)(ev + 20);
				ring_push(c, (1LL << 48) | ((int64_t)(sym & 0xFFFFFF) << 24));
			}
			else if (t == BZ_SDL_KEYUP)
			{
				int32_t sym = *(int32_t*)(ev + 20);
				ring_push(c, (2LL << 48) | ((int64_t)(sym & 0xFFFFFF) << 24));
			}
			else if (t == BZ_SDL_MOUSEMOTION)
			{
				int32_t x = *(int32_t*)(ev + 20), y = *(int32_t*)(ev + 24);
				ring_push(c, (3LL << 48) | ((int64_t)(x & 0xFFFFFF) << 24) | (y & 0xFFFFFF));
			}
			else if (t == BZ_SDL_MOUSEBUTTONDOWN)
			{
				uint8_t b = *(uint8_t*)(ev + 16);
				int32_t x = *(int32_t*)(ev + 20);
				ring_push(c, (4LL << 48) | ((int64_t)(x & 0xFFFFFF) << 24) | b);
			}
		}

		if (c->frame_ready)
		{
			bzy_mutex_lock(&c->lock);
			uint32_t *tmp = c->front;
			c->front = c->back;
			c->back = tmp;
			c->frame_ready = 0;
			bzy_mutex_unlock(&c->lock);

			sdl.UpdateTexture(c->tex, NULL, c->front, c->w * 4);
			sdl.RenderClear(c->ren);
			sdl.RenderCopy(c->ren, c->tex, NULL, NULL);
			sdl.RenderPresent(c->ren);
		}

		bzy_sem_wait_ms(&c->frame_sem, 2);   /* Wake on present, else pump events every ~2ms. */
	}

	sdl.DestroyTexture(c->tex);
	sdl.DestroyRenderer(c->ren);
	sdl.DestroyWindow(c->win);
	sdl.Quit();
	return NULL;
}

/* Offloaded so the breeze parks (frees its scheduler core) while the window inits. */
static void surface_wait_open(void *p)
{
	bzy_sem_wait(&((SurfaceCtrl*)p)->open_sem);
}

static void surface_teardown(SurfaceCtrl *c)
{
	c->stop = 1;
	bzy_sem_post(&c->frame_sem, 1);
	if (c->started)
	{
		bzy_thread_join(c->thread);
	}
	bzy_sem_destroy(&c->open_sem);
	bzy_sem_destroy(&c->frame_sem);
	free(c->front);
	free(c->back);
	free(c);
}

static void surface_finalize(void *o)
{
	if (SURF_CLOSED(o))
	{
		return;
	}
	if (SURF_CTRL(o))
	{
		surface_teardown(SURF_CTRL(o));
		SURF_CTRL(o) = NULL;
	}
	SURF_CLOSED(o) = 1;
}

static void *surface_vtable(void)
{
	g_surf_typeinfo[0] = (int64_t)(void*)surface_finalize;
	g_surf_vtable[0] = (int64_t)&g_surf_typeinfo[0];
	return &g_surf_vtable[1];
}

void *bzy_surface_open(int64_t w, int64_t h, void *title)
{
	if (!surface_load())
	{
		return NULL;
	}
	if (w <= 0 || h <= 0)
	{
		bzy_io_fail("Surface.open: width and height must be positive.");
		return NULL;
	}

	SurfaceCtrl *c = (SurfaceCtrl*)calloc(1, sizeof(SurfaceCtrl));
	if (!c)
	{
		bzy_io_fail("Surface.open: out of memory.");
		return NULL;
	}
	c->w = (int)w;
	c->h = (int)h;
	const char *tt = title ? bzy_str_data(title) : "";
	strncpy(c->title, tt, sizeof(c->title) - 1);
	c->front = (uint32_t*)malloc((size_t)w * (size_t)h * 4);
	c->back  = (uint32_t*)malloc((size_t)w * (size_t)h * 4);
	if (!c->front || !c->back)
	{
		free(c->front);
		free(c->back);
		free(c);
		bzy_io_fail("Surface.open: out of memory.");
		return NULL;
	}
	bzy_mutex_init(&c->lock);
	bzy_sem_init(&c->open_sem);
	bzy_sem_init(&c->frame_sem);

	bzy_thread_start(&c->thread, surface_thread, c);
	c->started = 1;
	bzy_offload_run(surface_wait_open, c);   /* Park the breeze until the window is ready or failed. */

	if (c->init_failed)
	{
		surface_teardown(c);
		bzy_io_fail("Surface unavailable: SDL2 init failed or no display.");
		return NULL;
	}

	void *o = bzy_alloc(40);
	*(void**)o = surface_vtable();
	SURF_CTRL(o) = c;
	SURF_CLOSED(o) = 0;
	bzy_share_crosscore(o);   /* The handle is used from a breeze that may migrate cores. */
	return o;
}

void bzy_surface_present(void *s, void *pixels)
{
	if (SURF_CLOSED(s))
	{
		bzy_io_fail("Surface.present: surface is closed.");
		return;
	}
	SurfaceCtrl *c = SURF_CTRL(s);
	int64_t n = bzy_array_len(pixels);
	if (n != (int64_t)c->w * (int64_t)c->h)
	{
		bzy_io_fail("Surface.present: framebuffer length must be width*height.");
		return;
	}
	bzy_mutex_lock(&c->lock);
	memcpy(c->back, (char*)pixels + 32, (size_t)c->w * (size_t)c->h * 4);   /* Packed int[] payload at +32. */
	c->frame_ready = 1;
	bzy_mutex_unlock(&c->lock);
	bzy_sem_post(&c->frame_sem, 1);
}

int64_t bzy_surface_poll_event(void *s)
{
	if (SURF_CLOSED(s))
	{
		return 0;
	}
	SurfaceCtrl *c = SURF_CTRL(s);
	if (c->ring_tail == c->ring_head)
	{
		return 0;
	}
	int64_t v = c->ring[c->ring_tail & 255];
	c->ring_tail++;
	return v;
}

int64_t bzy_surface_is_open(void *s)
{
	if (SURF_CLOSED(s))
	{
		return 0;
	}
	return SURF_CTRL(s)->open ? 1 : 0;
}

void bzy_surface_close(void *s)
{
	if (SURF_CLOSED(s))
	{
		return;
	}
	surface_teardown(SURF_CTRL(s));
	SURF_CTRL(s) = NULL;
	SURF_CLOSED(s) = 1;
}
