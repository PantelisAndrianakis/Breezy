#ifndef AST_H
#define AST_H

typedef enum
{
	TY_VOID,
	TY_BOOL,
	TY_BYTE,  TY_SHORT,  TY_INT,  TY_LONG,    /* Signed. */
	TY_UBYTE, TY_USHORT, TY_UINT, TY_ULONG,   /* Unsigned. */
	TY_FLOAT, TY_DOUBLE,                      /* IEEE-754, signed only. */
	TY_F64X2,                                 /* SIMD: two packed f64 lanes in one xmm (16-byte value). */
	TY_F32X4,                                 /* SIMD: four packed f32 lanes in one xmm (16-byte value). */
	TY_I32X4,                                 /* SIMD: four packed i32 lanes in one xmm (16-byte value). */
	TY_F64X4,                                 /* SIMD: four packed f64 lanes in one ymm (32-byte AVX value). */
	TY_ARRAY,                                 /* T[]: 8-byte pointer to a heap array. */
	TY_MAP,                                   /* map<K,V>: 8-byte pointer to a heap map. */
	TY_GENERIC,                               /* Box<T> etc.: 8-byte pointer to a heap object. */
	TY_ENTRY,                                 /* map Entry: 8-byte pointer; elem=K, elem2=V. */
	TY_CHANNEL,                               /* channel<T>: 8-byte pointer; elem=T. */
	TY_TIMER,                                 /* Timer: 8-byte pointer to a heap timer handle. */
	TY_LISTENER,                              /* Listener: TCP accept handle. */
	TY_SOCKET,                                /* Socket: TCP connection handle. */
	TY_TLSSOCKET,                             /* TlsSocket: TLS connection handle. */
	TY_TLSLISTENER,                           /* TlsListener: TLS accept handle. */
	TY_SURFACE,                               /* Surface: SDL2 pixel-window handle. */
	TY_GLSURFACE,                             /* GlSurface: SDL2 OpenGL context handle. */
	TY_UDPSOCKET,                             /* UdpSocket: UDP datagram handle. */
	TY_DATAGRAM,                              /* Datagram: a received UDP payload + sender. */
	TY_FILECHANNEL,                           /* FileChannel: random-access file handle. */
	TY_FILEWRITER,                            /* FileWriter: buffered file-write handle. */
	TY_MAPPEDFILE,                            /* MappedFile: memory-mapped file handle. */
	TY_LOGGER,                                /* Logger: channel-fed buffered log handle. */
	TY_XMLNODE,                               /* XmlNode: a parsed XML element node (managed tree). */
	TY_JSONVALUE,                             /* JsonValue: a parsed JSON value node (managed tagged tree). */
	TY_HTTPREQUEST,                           /* HttpRequest: a parsed/built HTTP request message (managed). */
	TY_HTTPRESPONSE,                          /* HttpResponse: a parsed/built HTTP response message (managed). */
	TY_OBJECT,
	TY_STRING,  /* Immutable string. */
	TY_NULL,    /* The `null` literal: a bare 0 assignable to any managed reference. */
	TY_FUNC     /* FFI: C function-pointer type; elem=return type, targs[0..targ_count)=param types. */
} TypeKind;
typedef struct TypeRef
{
	TypeKind kind;
	char class_name[64];
	struct TypeRef *elem;    /* TY_ARRAY element, or TY_MAP key; NULL otherwise. */
	struct TypeRef *elem2;   /* TY_MAP value; NULL otherwise. */
	struct TypeRef **targs;  /* User-generic application args (TY_GENERIC, user class). Built once
	                            at parse; value-copies share it read-only (typeref_deepcopy clones it). */
	int targ_count;          /* 0 for built-in templates (Box/List/...), which use elem/elem2. */
	int targ_cap;
} TypeRef;

/* Width in bits of a scalar kind. A bool reports 1; objects/strings are
 * 64-bit pointers; void has no width and reports 0. */
static inline int ty_bits(TypeKind k)
{
	switch (k)
	{
	case TY_BOOL:
		return 1;
	case TY_BYTE:
	case TY_UBYTE:
		return 8;
	case TY_SHORT:
	case TY_USHORT:
		return 16;
	case TY_INT:
	case TY_UINT:
		return 32;
	case TY_LONG:
	case TY_ULONG:
		return 64;
	case TY_FLOAT:
		return 32;
	case TY_DOUBLE:
		return 64;
	case TY_F64X2:
	case TY_F32X4:
	case TY_I32X4:
		return 128;
	case TY_F64X4:
		return 256;
	case TY_ARRAY:
	case TY_MAP:
	case TY_GENERIC:
	case TY_ENTRY:
	case TY_CHANNEL:
	case TY_TIMER:
	case TY_LISTENER:
	case TY_SOCKET:
	case TY_TLSSOCKET:
	case TY_TLSLISTENER:
	case TY_SURFACE:
	case TY_GLSURFACE:
	case TY_UDPSOCKET:
	case TY_DATAGRAM:
	case TY_FILECHANNEL:
	case TY_FILEWRITER:
	case TY_MAPPEDFILE:
	case TY_LOGGER:
	case TY_XMLNODE:
	case TY_JSONVALUE:
	case TY_HTTPREQUEST:
	case TY_HTTPRESPONSE:
	case TY_OBJECT:
	case TY_STRING:
	case TY_NULL:
	case TY_FUNC:
		return 64;
	case TY_VOID:
		return 0;
	}

	return 0;
}

/* True for any integer kind (byte..ulong); excludes bool. */
static inline int ty_is_int(TypeKind k)
{
	return k == TY_BYTE || k == TY_SHORT || k == TY_INT || k == TY_LONG
	       || k == TY_UBYTE || k == TY_USHORT || k == TY_UINT || k == TY_ULONG;
}

/* True for the signed integer kinds. */
static inline int ty_is_signed(TypeKind k)
{
	return k == TY_BYTE || k == TY_SHORT || k == TY_INT || k == TY_LONG;
}

/* True for the unsigned integer kinds. */
static inline int ty_is_unsigned(TypeKind k)
{
	return k == TY_UBYTE || k == TY_USHORT || k == TY_UINT || k == TY_ULONG;
}

/* True for the floating-point kinds. */
static inline int ty_is_float(TypeKind k)
{
	return k == TY_FLOAT || k == TY_DOUBLE;
}

/* True for the packed SIMD vector kinds: 16-byte values that live in a full xmm
   register, NOT scalar floats. Deliberately excluded from ty_is_float so the
   scalar double codegen (movsd, cvtss2sd, promotion) never touches them. */
static inline int ty_is_simd(TypeKind k)
{
	return k == TY_F64X2 || k == TY_F32X4 || k == TY_I32X4 || k == TY_F64X4;
}

/* The stack-slot / move width of a SIMD value in bytes: 16 for the SSE xmm
   types, 32 for the 256-bit AVX ymm types. */
static inline int ty_simd_bytes(TypeKind k)
{
	return k == TY_F64X4 ? 32 : 16;
}

/* True for ARC-managed, heap, 8-byte-pointer kinds (objects and strings). Use
   this — not a bare `== TY_OBJECT` — wherever a retain/release decision is made. */
static inline int ty_is_managed(TypeKind k)
{
	return k == TY_OBJECT || k == TY_STRING || k == TY_ARRAY || k == TY_MAP || k == TY_GENERIC || k == TY_ENTRY || k == TY_CHANNEL || k == TY_TIMER
	       || k == TY_LISTENER || k == TY_SOCKET || k == TY_UDPSOCKET || k == TY_DATAGRAM
	       || k == TY_TLSSOCKET || k == TY_TLSLISTENER || k == TY_SURFACE || k == TY_GLSURFACE
	       || k == TY_FILECHANNEL || k == TY_FILEWRITER || k == TY_MAPPEDFILE || k == TY_LOGGER
	       || k == TY_XMLNODE || k == TY_JSONVALUE || k == TY_HTTPREQUEST || k == TY_HTTPRESPONSE
	       || k == TY_FUNC;   /* A first-class function value is a managed closure object (FFI callbacks pass a bare address via is_func_addr, never a closure). */
}

/* Width rank for implicit widening: 8 < 16 < 32 < 64. Non-integers rank 0. */
static inline int ty_rank(TypeKind k)
{
	return ty_is_int(k) ? ty_bits(k) : 0;
}

/* The unsigned integer kind of the same width. Unsigned and non-integer kinds
   are returned unchanged. Used for C-style "unsigned wins" mixed-sign promotion. */
static inline TypeKind ty_to_unsigned(TypeKind k)
{
	switch (k)
	{
	case TY_BYTE:
		return TY_UBYTE;
	case TY_SHORT:
		return TY_USHORT;
	case TY_INT:
		return TY_UINT;
	case TY_LONG:
		return TY_ULONG;
	default:
		return k;
	}
}

typedef enum
{
	EX_INT, EX_BOOL, EX_FLOAT, EX_STR, EX_IDENT, EX_THIS, EX_NEW, EX_NEWARRAY,
	EX_NEWMAP, EX_NEWGEN, EX_NEWCHANNEL, EX_BINARY, EX_UNARY, EX_INCDEC, EX_CAST, EX_CALL, EX_METHOD_CALL, EX_FIELD, EX_INDEX, EX_NULL, EX_LAMBDA,
	EX_TRYOP        /* `operand?`: unwrap Some/Ok, else early-return None/Err. Operand in lhs. */
} ExprKind;

/* Lambda literal (EX_LAMBDA) side-data, kept off the Expr hot struct. A lambda
   lowers to a synthetic function body (taking its closure environment as a hidden
   arg0) plus a closure-object allocation at the literal site. */
typedef struct LambdaParam
{
	char    name[64];
	TypeRef type;             /* Valid when has_type; else inferred at resolve. */
	int     has_type;
} LambdaParam;

typedef struct LambdaCap
{
	char    name[64];         /* Enclosing local captured by value. */
	TypeRef type;
	int     src_offset;       /* Resolver: stack slot of the source local (enclosing frame). */
	int     env_offset;       /* Codegen: byte offset of this capture in the env object. */
	int     local_slot;       /* Resolver: stack slot inside the body, seeded from env at entry. */
	int     is_managed;       /* 1 if the captured value is a managed reference. */
} LambdaCap;

typedef struct LambdaInfo
{
	LambdaParam params[16];
	int         param_count;
	int         is_block;     /* 1: body is a Block; 0: body is a single expression. */
	struct Expr  *body_expr;
	struct Block *body_block;
	LambdaCap   caps[32];
	int         cap_count;
	char        label[64];    /* Resolver: synthetic body label (__lambda_N). */
	TypeRef     sig;          /* Resolver: the TY_FUNC signature this lambda satisfies. */
	struct Func *sf;          /* Resolver: the synthesized body function (env arg0 + params). */
} LambdaInfo;

typedef struct Expr Expr;
struct Expr
{
	ExprKind kind;
	int      line;
	TypeRef  type;            /* Resolver: result type. */
	int      anno_int;        /* Resolver: stack offset / field offset / vtable slot. */
	char     anno_str[64];    /* Resolver: static class for dispatch. */
	int      anno_stack;      /* EX_NEW: 1 if stack-allocated, else 0 (heap). */
	int      anno_stack_off;  /* EX_NEW: rbp offset of the stack object when anno_stack. */
	long long int_val;        /* EX_INT (64-bit: 'long' is 32-bit on Win64). */
	char     int_suffix[4];   /* EX_INT/EX_FLOAT: literal suffix from the lexer ("", "L", "u", "uL", "Lu", "f"). */
	double   float_val;       /* EX_FLOAT: parsed literal value. */
	char     str_val[256];    /* EX_STR: decoded string-literal bytes. */
	char     name[64];        /* EX_IDENT/NEW/CALL/METHOD_CALL/FIELD. */
	int      op;              /* EX_BINARY/EX_UNARY: a TokenType. */
	Expr    *lhs;             /* Binary left / unary operand / method-call|field receiver. */
	Expr    *rhs;             /* Binary right. */
	Expr   **args;            /* Call / method call; grown via expr_add_arg. */
	int      arg_count;
	int      arg_cap;
	int      anno_nonneg;     /* Non-neg pass: 1 if this /,% node's dividend is provably >= 0. */
	int      anno_index_safe; /* BCE pass: 1 if this EX_INDEX's index is provably in [0, length). */
	long long anno_len_const; /* BCE pass: this EX_INDEX array's exact length when statically constant (new T[N]); 0 = unknown. A kept bounds check then compares against the immediate, not a memory load of [base+24]. */
	int      anno_shared_gate;/* Resolver: EX_INDEX on a managed element of a maybe-shared array -> SHARED-bit gated access (owned result). */
	int      anno_overload;   /* Resolver: index of the selected overload (ctor/method/func) within its set; default 0. */
	int      is_func_addr;    /* FFI: this arg is a bare function name passed as a C function pointer (its address). */
	int      anno_indirect;   /* Resolver: EX_CALL through a function value (closure), not a named function. */
	int      anno_capture;    /* Resolver: EX_IDENT that reads a captured var from the closure env (anno_int = env byte offset). */
	LambdaInfo *lam;          /* EX_LAMBDA side-data. */
};

typedef enum { ST_VARDECL, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN, ST_EXPR, ST_FOREACH, ST_BREAK, ST_CONTINUE, ST_FOR, ST_SWITCH, ST_CASE, ST_DEFAULT, ST_THROW, ST_TRY, ST_CATCH, ST_SPAWN, ST_SELECT, ST_ASM } StmtKind;

/* One arm of a `select`. A receive arm binds the value (`v = ch.receive() => ...`);
   a send arm delivers a value (`ch.send(x) => ...`). */
typedef struct
{
	int      is_send;       /* 1 = send arm, 0 = receive arm. */
	Expr    *chan;          /* The channel expression. */
	char     bind[64];      /* Receive arm: bound local name (empty if the value is discarded). */
	TypeRef  bind_type;     /* Receive arm: channel element type (resolved). */
	int      bind_offset;   /* Receive arm: frame slot for the bound local (resolved). */
	Expr    *send_val;      /* Send arm: the value expression. */
	struct Block *body;     /* The arm body (Block is defined below). */
} SelectArm;

typedef struct Stmt Stmt;
typedef struct Block Block;
struct Stmt
{
	StmtKind kind;
	int      line;
	TypeRef  decl_type;       /* ST_VARDECL. */
	char     decl_name[64];
	Expr    *decl_init;       /* May be NULL. */
	int      decl_offset;     /* Resolver: stack slot for this local. */
	int      fe_coll_offset;  /* ST_FOREACH: container pointer slot. */
	int      fe_index_offset; /* ST_FOREACH: index / map slot cursor. */
	int      fe_len_offset;   /* ST_FOREACH (string): precomputed length. */
	int      fe_aux_offset;   /* ST_FOREACH (string): precomputed data pointer. */
	TypeRef  fe_val_type;     /* ST_FOREACH pair form: value var type (kind TY_VOID if absent). */
	char     fe_val_name[64]; /* ST_FOREACH pair form: value var name. */
	int      fe_val_offset;   /* ST_FOREACH pair form: value var stack slot. */
	Stmt    *for_init;        /* ST_FOR: init clause (var-decl or assignment/expr). */
	Stmt    *for_post;        /* ST_FOR: post clause (assignment or ++/--). */
	Expr    *target;          /* ST_ASSIGN lvalue (EX_IDENT|EX_FIELD). */
	Expr    *value;           /* ST_ASSIGN rhs. */
	Expr    *cond;            /* ST_IF / ST_WHILE. */
	Block   *then_blk;        /* Then-branch / while-body. */
	Block   *else_blk;        /* Else-branch (NULL if none). */
	Expr    *ret_val;         /* ST_RETURN (may be NULL). */
	Expr    *expr;            /* ST_EXPR. */
	int      is_match;        /* ST_SWITCH built from `match`: exhaustiveness enforced, arms auto-break. */
	int      is_narrow;       /* ST_VARDECL: a match-arm tag-narrowing (base enum -> variant subclass);
	                            skip the downcast assignability check, the matched tag guarantees it. */
	char   (*case_binds)[64]; /* ST_CASE: positional payload bind names, e.g. Circle(r) -> {"r"}. */
	int      case_bind_count;
	SelectArm *sel_arms;      /* ST_SELECT: the send/receive arms. */
	int      sel_arm_count;   /* ST_SELECT: arm count (else_blk holds the default body, or NULL). */
	char   (*asm_lines)[256]; /* ST_ASM: raw assembly lines, emitted verbatim. */
	int      asm_line_count;
	Stmt    *accum_stmt;      /* P5: the recognized `s = s + ...` body statement, or NULL. */
	int      accum_sb_offset; /* P5: frame slot for the lowering StringBuilder (0 = not lowered). */
};

struct Block
{
	Stmt **stmts;
	int count;
	int cap;
};

typedef struct
{
	TypeRef type;
	char name[64];
	Expr *def;   /* Default value (a literal) used when the argument is omitted, or NULL. */
} Param;
typedef struct Func
{
	TypeRef ret_type;
	char name[64];
	Param  *params;           /* Grown via func_add_param; param_count live, param_cap allocated. */
	int param_count;
	int param_cap;
	Block  *body;
	int     is_extern;        /* FFI: declared with `extern`, no body; asm_label is the raw C symbol. */
	int     is_blocking;      /* FFI: `extern blocking` — dispatch via the offload pool. */
	int     is_variadic;      /* FFI: trailing `...` — a variadic C function (e.g. printf/snprintf). */
	int     is_dynamic;       /* FFI: `extern dynamic` — address resolved at run time via the registered resolver. */
	int     is_static;        /* Static method: no `this`, called via the class name. */
	int     is_lambda;        /* Synthesized lambda body (env arg0); kept off the IR path. */
	int     cap_count;        /* Lambda body: captures seeded from env at entry. */
	int     cap_env_off[32];  /* Lambda body: byte offset of each capture in the env object. */
	int     cap_local_off[32];/* Lambda body: stack slot each capture is copied into. */
	int     frame_size;       /* Resolver. */
	int     obj_local_offsets[64];  /* Ownership pass: the stack offset of each object-typed local. */
	int     obj_local_count;        /* Number of entries in obj_local_offsets. */
	int     stack_alloc_bytes;      /* Escape pass: total frame bytes reserved for stack objects. */
	int     max_temp_depth;         /* Frame pass: deepest simultaneously-live preserve-across-call temporaries. */
	int     max_outgoing_args;      /* Frame pass: most argument slots any call in this body needs (0..4). */
	int     max_scratch_bytes;      /* Frame pass: peak sum of nested scratch-block bytes (sizes the static-rsp arena). */
#define PROMO_MAX 64                /* Promotion: max logical locals (registers reused across disjoint live ranges). */
	int     promo_count;            /* Register promotion: number of promoted locals (<= PROMO_MAX; share 4 phys regs). */
	int     promo_off[PROMO_MAX];   /* Register promotion: rbp slot offset of each promoted local. */
	int     promo_reg[PROMO_MAX];   /* Register promotion: physical register index 0..3 -> r12..r15 (repeats under reuse). */
	int     fpromo_count;           /* Float promotion: number of promoted double locals (<= PROMO_MAX; share 4 xmm regs). */
	int     fpromo_off[PROMO_MAX];  /* Float promotion: rbp slot offset of each promoted double local. */
	int     fpromo_reg[PROMO_MAX];  /* Float promotion: physical register index 0..3 -> xmm2..xmm5 (repeats under reuse). */
} Func;

typedef struct
{
	TypeRef type;
	char name[64];
	int   is_static;          /* Static field: one shared global slot, not in the object. */
	Expr *init;               /* Static-field declaration initializer, or NULL. */
} Field;
typedef struct
{
	char  name[64];
	int is_static;            /* `static class`: every member is static, not instantiable. */
	int is_record;            /* `record`: final class with compiler-synthesized hashCode/equals. */
	char parent_name[64];
	int has_parent;
	struct TypeRef **parent_targs;  /* Parametric inheritance: type args on the parent, e.g.
	                                   `extends Box<T>`. NULL/0 for a non-generic parent. */
	int parent_targ_count;
	char (*implements)[64];   /* Interface names this class implements. Grown at parse. */
	int implements_count;
	int implements_cap;
	char (*type_params)[64];        /* Generic class: parameter names, e.g. "T". Grown at parse. */
	char (*type_param_bounds)[64];  /* Interface bound per param, or "" if none. */
	int  type_param_count;          /* 0 = ordinary (non-generic) class. */
	int  type_param_cap;
	int  type_param_bounds_cap;
	Field *fields;            /* Grown via class_add_field. */
	int field_count;
	int fields_cap;
	Func **methods;           /* Grown via class_add_method. */
	int method_count;
	int methods_cap;
	Func **ctors;             /* Overloaded constructors, declaration order; ret_type is TY_VOID. */
	int ctor_count;
	int ctors_cap;
	Func *ctor;               /* Legacy alias = ctors[0] when ctor_count >= 1, else NULL. */
} ClassDecl;

typedef struct
{
	char  name[64];
	Func **methods;           /* Bodyless signature Funcs (body == NULL). Grown at parse. */
	int   method_count;
	int   methods_cap;
} InterfaceDecl;

typedef struct
{
	char  name[64];           /* Constant name, e.g. "RED". */
	Expr *args[8];            /* Constructor arguments for this constant. */
	int   arg_count;
	Func *overrides[8];       /* Per-constant method override bodies (empty if none). */
	int   override_count;
	/* Payload variant (sum type): the constant declares its own typed fields,
	   e.g. `Circle(double r)`. payload_count>0 marks a constructible variant
	   (Shape.Circle(2.0) builds a fresh instance) rather than a singleton
	   constant. The fields become a subclass `Enum$Const` with a synthesized
	   constructor that sets __ordinal/__name and stores the payload. */
	Field payload[8];
	int   payload_count;
} EnumConstant;

typedef struct
{
	char  name[64];
	char  (*type_params)[64]; /* Generic enum: parameter names, e.g. "T". 0 = non-generic. */
	int   type_param_count;
	int   type_param_cap;
	char  (*implements)[64];
	int   implements_count;
	int   implements_cap;
	EnumConstant *constants;  /* Grown at parse. */
	int   constant_count;
	int   constants_cap;
	Field *fields;            /* Shared instance fields (user-declared). */
	int   field_count;
	int   fields_cap;
	Func **methods;           /* Shared instance methods. */
	int   method_count;
	int   methods_cap;
	Func *ctor;               /* The enum constructor (params + body), or NULL. */
} EnumDecl;

typedef struct
{
	ClassDecl    **klasses;       /* Peer top-level classes declared in this file. */
	int            class_count;
	int            class_cap;
	InterfaceDecl **interfaces;   /* File-scope interface declarations. Grown at parse. */
	int            interface_count;
	int            interfaces_cap;
	EnumDecl      **enums;        /* File-scope enum declarations. */
	int            enum_count;
	int            enums_cap;
	Func          **funcs;        /* File-scope functions. */
	int            func_count;
	int            funcs_cap;
} Unit;

void   ast_free_all(void);
Expr  *expr_new(ExprKind kind, int line);
LambdaInfo *lambda_new(void);
void   lambda_add_param(LambdaInfo *l, const char *name, int has_type, TypeRef type);
TypeRef *typeref_box(TypeRef t);
void     typeref_add_targ(TypeRef *f, TypeRef t);   /* Append a param type to a TY_FUNC signature. */
Stmt  *stmt_new(StmtKind kind, int line);
Block *block_new(void);
void   block_push(Block *b, Stmt *s);
Param *func_add_param(Func *f);          /* Grow f->params by one; returns the zeroed new slot. */
void   expr_add_arg(Expr *e, Expr *a);   /* Append a to e->args, growing as needed. */
Field *class_add_field(ClassDecl *c);    /* Grow c->fields by one; returns the zeroed new slot. */
void   class_add_method(ClassDecl *c, Func *m);
void   class_add_ctor(ClassDecl *c, Func *f);
Func  *func_new(void);
ClassDecl *class_new(void);
InterfaceDecl *interface_new(void);
EnumDecl *enum_new(void);
Unit  *unit_new(void);
void   unit_add_class(Unit *u, ClassDecl *c);

TypeRef    typeref_deepcopy(const TypeRef *t);   /* Deep copy incl. elem/elem2/targs. */
Expr      *expr_clone(const Expr *e);
Stmt      *stmt_clone(const Stmt *s);
Block     *block_clone(const Block *b);
Func      *func_clone(const Func *f);
ClassDecl *classdecl_clone(const ClassDecl *c);

#endif
