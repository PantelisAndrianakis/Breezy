#ifndef ENUMS_H
#define ENUMS_H
#include "ast.h"

typedef struct
{
	char name[64];                 /* Enum class name, e.g. "Color". */
	char (*implements)[64];
	int  implements_count;
	int  implements_cap;
	int  constant_count;
	int  constant_cap;             /* Capacity of the parallel per-constant arrays below. */
	char (*const_name)[64];        /* Constant names, ordinal order. */
	char (*const_class)[64];       /* Instantiated class: "Color" or "Color$RED". */
	Expr *(*const_args)[8];        /* Per-constant ctor args (AST pointers; inner [8] kept). */
	int  *const_argc;
	int  *const_payload;           /* 1 if this constant is a payload variant (constructed per
	                                  call via Enum$Const(...), not a startup singleton). */
	int  type_param_count;         /* >0 for a generic enum (enum Name<T>). */
	int  *const_payload_n;         /* Number of payload fields per constant (0 = a zero-field variant). */
} EnumInfo;

/* Lower every EnumDecl: synthesize the base class (+ per-constant subclasses for
   override bodies), append them as Units, clear Unit.enums, and record an
   EnumInfo per enum. Run BEFORE generics_expand. Grows *total in place. */
void enums_expand(Unit ***units, int *total, int *cap);

/* Registry queries (valid after enums_expand). */
int          enum_is(const char *name);                   /* 1 if name is an enum type. */
int          enum_ordinal(const char *en, const char *c); /* ordinal or -1. */
int          enum_count_of(const char *en);
const char  *enum_const_name(const char *en, int idx);
int          enum_is_generic(const char *en);                      /* 1 if the enum has type parameters. */
/* Register a monomorphized enum instance (e.g. "Wrap$int") as an enum mirroring
   its generic base, so match/ordinal/name recognize it. `suffix` is the arg part
   of the instance name (e.g. "$int"); payload variant classes become
   base-variant + suffix (Wrap$Of -> Wrap$Of$int). */
void         enum_register_instance(const char *inst, const char *base, const char *suffix);
int          enum_const_is_payload(const char *en, const char *c); /* 1 if constant c is a payload variant. */
int          enum_variant_field_count(const char *en, const char *c); /* Payload field count, or -1 if not a variant. */
const char  *enum_variant_class(const char *en, const char *c);    /* "Enum$C" for a payload variant, else NULL. */
int          enum_total(void);
const EnumInfo *enum_at(int i);

#endif
