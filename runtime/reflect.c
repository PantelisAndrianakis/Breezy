#include "breezy.h"
#include <string.h>

/* Walk obj -> vtable -> (vtable-8) descriptor -> [2 + nobj] class-name pointer.
   The descriptor is [finalizer][nobj][offsets...][name][backptr]; the name word
   sits just past the nobj offsets (see codegen cg_emit_vtable). Reads the object's
   actual vtable, so the result is the dynamic (most-derived) class. */
void *bzy_class_name(void *obj)
{
	void *vtable = *(void**)obj;
	int64_t *desc = *(int64_t**)((char*)vtable - 8);
	int64_t nobj = desc[1];
	const char *name = *(const char**)(desc + 2 + nobj);
	return bzy_str_new(name, (int64_t)strlen(name));
}
