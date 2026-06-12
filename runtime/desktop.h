/* Desktop GUI runtime: C entry points called by the Breezy desktop prelude.
   Every symbol here marshals to a single dedicated GTK thread; GTK itself is
   loaded dynamically (dlopen/LoadLibrary) so this object adds no link-time GTK
   dependency. Handles are opaque GtkWidget pointers returned as int64_t. */
#ifndef BZY_DESKTOP_H
#define BZY_DESKTOP_H

#include <stdint.h>

/* Marshalling widths matter: Breezy `long` -> int64_t (handles), Breezy
   `int`/`bool` -> 32-bit `int`. Mixing them (e.g. an int64_t param for a Breezy
   `int` arg) reads garbage in the high bits, so the widths below mirror the
   Breezy extern declarations in src/prelude.c exactly. */

/* Lifecycle / capability. */
int bzy_desktop_is_enabled(void);   /* 1 if GTK loadable and a display exists, else 0 (Breezy bool). */

/* Top-level windows. */
int64_t bzy_desktop_frame_new(void);                       /* Returns a window handle. */
int64_t bzy_desktop_frame_content(int64_t frame);          /* The window's border container (== frame). */
void    bzy_desktop_frame_set_title(int64_t frame, const char *title);

/* Containers / controls. */
int64_t bzy_desktop_panel_new(void);
int64_t bzy_desktop_button_new(void);
void    bzy_desktop_button_set_text(int64_t btn, const char *text);
int64_t bzy_desktop_label_new(void);
void    bzy_desktop_label_set_text(int64_t lbl, const char *text);

/* Tree / visibility / state. */
void bzy_desktop_container_add(int64_t parent, int64_t child);             /* Plain append (vbox). */
void bzy_desktop_border_add(int64_t border, int64_t child, int region);   /* 0=N 1=S 2=W 3=E 4=C. */
void bzy_desktop_set_visible(int64_t widget, int visible);
void bzy_desktop_set_enabled(int64_t widget, int enabled);
void bzy_desktop_window_set_size(int64_t window, int w, int h);
void bzy_desktop_window_show(int64_t window);
void bzy_desktop_window_dispose(int64_t window);

/* Event wiring. The registration index identifies the Breezy widget object so
   the Breezy run-loop can dispatch without C calling back into Breezy. */
void bzy_desktop_listen_action(int64_t widget, int reg_index);        /* "clicked". */
void bzy_desktop_listen_window_close(int64_t window, int reg_index);  /* "destroy". */

/* Event pump (blocking). Returns the reg_index of the next event's source, or
   -1 when the last window has closed (run-loop should stop). The companion
   call reports the kind of the event just returned (0=action, 1=window-close).
   Both return Breezy `int` (32-bit). */
int bzy_desktop_next_event(void);
int bzy_desktop_event_kind(void);

#endif
