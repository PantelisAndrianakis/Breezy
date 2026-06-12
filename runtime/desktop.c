/* Desktop GUI runtime. GTK3 is resolved at runtime (no compile/link-time GTK
   dependency), so a program that never touches the Desktop API pulls this
   object only if its prelude references __desktop_* — and even then needs no
   GTK to link. All GTK work happens on one dedicated thread (see Task 3). */
#include "desktop.h"

#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		HMODULE h = LoadLibraryA(names[i]);
		if (h)
		{
			return (void *)h;
		}
	}
	return NULL;
}
static void *dl_sym(void *h, const char *name)
{
	return (void *)GetProcAddress((HMODULE)h, name);
}
#else
#include <dlfcn.h>
#include <stdlib.h>
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
static void *dl_sym(void *h, const char *name)
{
	return dlsym(h, name);
}
#endif

/* The subset of GTK3/GLib used by Phase 1, as function pointers. void* stands in
   for every GTK pointer type so this file needs no GTK headers. */
typedef int  (*fn_init_check)(int *argc, char ***argv);
typedef void (*fn_main)(void);
typedef void (*fn_main_quit)(void);
typedef void *(*fn_window_new)(int type);
typedef void  (*fn_window_set_title)(void *w, const char *t);
typedef void  (*fn_window_set_default_size)(void *w, int width, int height);
typedef void *(*fn_button_new)(void);
typedef void  (*fn_button_set_label)(void *b, const char *t);
typedef void *(*fn_label_new)(const char *t);
typedef void  (*fn_label_set_text)(void *l, const char *t);
typedef void *(*fn_box_new)(int orientation, int spacing);
typedef void  (*fn_box_pack_start)(void *box, void *child, int expand, int fill, unsigned padding);
typedef void  (*fn_container_add)(void *c, void *child);
typedef void  (*fn_widget_show_all)(void *w);
typedef void  (*fn_widget_set_visible)(void *w, int visible);
typedef void  (*fn_widget_set_sensitive)(void *w, int sensitive);
typedef void  (*fn_widget_destroy)(void *w);
typedef unsigned long (*fn_signal_connect_data)(void *instance, const char *sig,
		void (*cb)(void), void *data, void *destroy, int flags);
typedef unsigned (*fn_idle_add)(int (*fn)(void *), void *data);
typedef void *(*fn_async_queue_new)(void);
typedef void  (*fn_async_queue_push)(void *q, void *data);
typedef void *(*fn_async_queue_pop)(void *q);

struct gtk_api
{
	int loaded;          /* 0 = not attempted, 1 = ok, -1 = failed. */
	void *gtk;
	void *glib;
	void *gobject;
	fn_init_check init_check;
	fn_main main;
	fn_main_quit main_quit;
	fn_window_new window_new;
	fn_window_set_title window_set_title;
	fn_window_set_default_size window_set_default_size;
	fn_button_new button_new;
	fn_button_set_label button_set_label;
	fn_label_new label_new;
	fn_label_set_text label_set_text;
	fn_box_new box_new;
	fn_box_pack_start box_pack_start;
	fn_container_add container_add;
	fn_widget_show_all widget_show_all;
	fn_widget_set_visible widget_set_visible;
	fn_widget_set_sensitive widget_set_sensitive;
	fn_widget_destroy widget_destroy;
	fn_signal_connect_data signal_connect_data;
	fn_idle_add idle_add;
	fn_async_queue_new async_queue_new;
	fn_async_queue_push async_queue_push;
	fn_async_queue_pop async_queue_pop;
};

static struct gtk_api G;

/* Resolve the GTK/GLib symbol set once. Returns 1 on success, 0 on failure.
   Idempotent and cheap on repeat calls. */
static int desktop_load(void)
{
	if (G.loaded != 0)
	{
		return G.loaded == 1;
	}

#if defined(_WIN32)
	static const char *gtk_names[]     = { "libgtk-3-0.dll", "gtk-3.dll", NULL };
	static const char *glib_names[]    = { "libglib-2.0-0.dll", "glib-2.0.dll", NULL };
	static const char *gobject_names[] = { "libgobject-2.0-0.dll", "gobject-2.0.dll", NULL };
#elif defined(__APPLE__)
	static const char *gtk_names[]     = { "libgtk-3.0.dylib", "libgtk-3.dylib", NULL };
	static const char *glib_names[]    = { "libglib-2.0.0.dylib", "libglib-2.0.dylib", NULL };
	static const char *gobject_names[] = { "libgobject-2.0.0.dylib", "libgobject-2.0.dylib", NULL };
#else
	static const char *gtk_names[]     = { "libgtk-3.so.0", "libgtk-3.so", NULL };
	static const char *glib_names[]    = { "libglib-2.0.so.0", "libglib-2.0.so", NULL };
	static const char *gobject_names[] = { "libgobject-2.0.so.0", "libgobject-2.0.so", NULL };
#endif

	G.gtk     = dl_open_first(gtk_names);
	G.glib    = dl_open_first(glib_names);
	G.gobject = dl_open_first(gobject_names);
	if (!G.gtk || !G.glib || !G.gobject)
	{
		G.loaded = -1;
		return 0;
	}

	G.init_check             = (fn_init_check)dl_sym(G.gtk, "gtk_init_check");
	G.main                   = (fn_main)dl_sym(G.gtk, "gtk_main");
	G.main_quit              = (fn_main_quit)dl_sym(G.gtk, "gtk_main_quit");
	G.window_new             = (fn_window_new)dl_sym(G.gtk, "gtk_window_new");
	G.window_set_title       = (fn_window_set_title)dl_sym(G.gtk, "gtk_window_set_title");
	G.window_set_default_size= (fn_window_set_default_size)dl_sym(G.gtk, "gtk_window_set_default_size");
	G.button_new             = (fn_button_new)dl_sym(G.gtk, "gtk_button_new");
	G.button_set_label       = (fn_button_set_label)dl_sym(G.gtk, "gtk_button_set_label");
	G.label_new              = (fn_label_new)dl_sym(G.gtk, "gtk_label_new");
	G.label_set_text         = (fn_label_set_text)dl_sym(G.gtk, "gtk_label_set_text");
	G.box_new                = (fn_box_new)dl_sym(G.gtk, "gtk_box_new");
	G.box_pack_start         = (fn_box_pack_start)dl_sym(G.gtk, "gtk_box_pack_start");
	G.container_add          = (fn_container_add)dl_sym(G.gtk, "gtk_container_add");
	G.widget_show_all        = (fn_widget_show_all)dl_sym(G.gtk, "gtk_widget_show_all");
	G.widget_set_visible     = (fn_widget_set_visible)dl_sym(G.gtk, "gtk_widget_set_visible");
	G.widget_set_sensitive   = (fn_widget_set_sensitive)dl_sym(G.gtk, "gtk_widget_set_sensitive");
	G.widget_destroy         = (fn_widget_destroy)dl_sym(G.gtk, "gtk_widget_destroy");
	G.signal_connect_data    = (fn_signal_connect_data)dl_sym(G.gobject, "g_signal_connect_data");
	G.idle_add               = (fn_idle_add)dl_sym(G.glib, "g_idle_add");
	G.async_queue_new        = (fn_async_queue_new)dl_sym(G.glib, "g_async_queue_new");
	G.async_queue_push       = (fn_async_queue_push)dl_sym(G.glib, "g_async_queue_push");
	G.async_queue_pop        = (fn_async_queue_pop)dl_sym(G.glib, "g_async_queue_pop");

	if (!G.init_check || !G.main || !G.window_new || !G.button_new || !G.box_new
		|| !G.signal_connect_data || !G.idle_add || !G.async_queue_new)
	{
		G.loaded = -1;
		return 0;
	}
	G.loaded = 1;
	return 1;
}

/* A display exists if GTK is loadable and (on X11/Wayland) a server is named.
   On Windows an interactive session always has a desktop. We deliberately do
   NOT call gtk_init_check here (it would touch the display from a worker
   thread); the environment probe is sufficient and side-effect-free. */
int bzy_desktop_is_enabled(void)
{
	if (!desktop_load())
	{
		return 0;
	}
#if defined(_WIN32) || defined(__APPLE__)
	return 1;
#else
	if (getenv("WAYLAND_DISPLAY") || getenv("DISPLAY"))
	{
		return 1;
	}
	return 0;
#endif
}

/* ---- Stubs filled in by Task 3 (widgets) and Task 5 (events). ---- */
int64_t bzy_desktop_frame_new(void) { return 0; }
int64_t bzy_desktop_frame_content(int64_t frame) { return frame; }
void bzy_desktop_frame_set_title(int64_t frame, const char *title) { (void)frame; (void)title; }
int64_t bzy_desktop_panel_new(void) { return 0; }
int64_t bzy_desktop_button_new(void) { return 0; }
void bzy_desktop_button_set_text(int64_t b, const char *t) { (void)b; (void)t; }
int64_t bzy_desktop_label_new(void) { return 0; }
void bzy_desktop_label_set_text(int64_t l, const char *t) { (void)l; (void)t; }
void bzy_desktop_container_add(int64_t p, int64_t c) { (void)p; (void)c; }
void bzy_desktop_border_add(int64_t b, int64_t c, int r) { (void)b; (void)c; (void)r; }
void bzy_desktop_set_visible(int64_t w, int v) { (void)w; (void)v; }
void bzy_desktop_set_enabled(int64_t w, int e) { (void)w; (void)e; }
void bzy_desktop_window_set_size(int64_t w, int a, int b) { (void)w; (void)a; (void)b; }
void bzy_desktop_window_show(int64_t w) { (void)w; }
void bzy_desktop_window_dispose(int64_t w) { (void)w; }
void bzy_desktop_listen_action(int64_t w, int i) { (void)w; (void)i; }
void bzy_desktop_listen_window_close(int64_t w, int i) { (void)w; (void)i; }
int bzy_desktop_next_event(void) { return -1; }
int bzy_desktop_event_kind(void) { return 0; }
