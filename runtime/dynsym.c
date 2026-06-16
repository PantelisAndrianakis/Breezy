/* Runtime-bound extern resolution. Own TU: a program with no `extern dynamic` and
   no Ffi.bind links none of this. The active resolver is a single function pointer;
   Ffi.bind registers a dlopen+dlsym resolver, and Graphics.openGL registers
   SDL_GL_GetProcAddress through the same bzy_dyn_set_resolver. */
#include "breezy.h"
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

static void *(*g_resolver)(const char *name) = NULL;

void bzy_dyn_set_resolver(void *(*r)(const char *name))
{
	g_resolver = r;
}

void *bzy_dynsym(const char *name)
{
	void *p = g_resolver ? g_resolver(name) : NULL;
	if (!p)
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "dynamic extern '%s' unresolved.", name ? name : "?");
		bzy_io_fail(msg);   /* Copies the message; throws via the call site's io_check. */
	}
	return p;
}

/* The dlsym resolver bound by Ffi.bind (one active library handle). */
#ifdef _WIN32
static HMODULE g_lib = NULL;
static void *dlsym_resolver(const char *name) { return g_lib ? (void*)GetProcAddress(g_lib, name) : NULL; }
int64_t bzy_ffi_bind(void *path)
{
	HMODULE h = LoadLibraryA(bzy_str_data(path));
	if (!h) { return 0; }
	g_lib = h;
	bzy_dyn_set_resolver(dlsym_resolver);
	return 1;
}
#else
static void *g_lib = NULL;
static void *dlsym_resolver(const char *name) { return g_lib ? dlsym(g_lib, name) : NULL; }
int64_t bzy_ffi_bind(void *path)
{
	void *h = dlopen(bzy_str_data(path), RTLD_NOW | RTLD_GLOBAL);
	if (!h) { return 0; }
	g_lib = h;
	bzy_dyn_set_resolver(dlsym_resolver);
	return 1;
}
#endif
