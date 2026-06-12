#ifndef PRELUDE_H
#define PRELUDE_H

/* Built-in Breezy classes always compiled into every program (vectors, etc.).
   Each entry is one class's source (one class per unit). */
extern const char *BZY_PRELUDE[];
extern const int   BZY_PRELUDE_COUNT;

/* The Desktop GUI prelude: injected by the driver ONLY when user source
   references the `Desktop` identifier, so non-GUI programs reserve none of
   these names and pull no GTK-bearing runtime object. */
extern const char *BZY_DESKTOP_PRELUDE[];
extern const int   BZY_DESKTOP_PRELUDE_COUNT;

#endif
