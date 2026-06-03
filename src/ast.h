#ifndef AST_H
#define AST_H

typedef enum
{
	TY_VOID,
	TY_BOOL,
	TY_BYTE,  TY_SHORT,  TY_INT,  TY_LONG,    /* Signed. */
	TY_UBYTE, TY_USHORT, TY_UINT, TY_ULONG,   /* Unsigned. */
	TY_FLOAT, TY_DOUBLE,                      /* IEEE-754, signed only. */
	TY_ARRAY,                                 /* T[]: 8-byte pointer to a heap array. */
	TY_MAP,                                   /* map<K,V>: 8-byte pointer to a heap map. */
	TY_GENERIC,                               /* Box<T> etc.: 8-byte pointer to a heap object. */
	TY_ENTRY,                                 /* map Entry: 8-byte pointer; elem=K, elem2=V. */
	TY_CHANNEL,                               /* channel<T>: 8-byte pointer; elem=T. */
	TY_TIMER,                                 /* Timer: 8-byte pointer to a heap timer handle. */
	TY_LISTENER,                              /* Listener: TCP accept handle. */
	TY_SOCKET,                                /* Socket: TCP connection handle. */
	TY_UDPSOCKET,                             /* UdpSocket: UDP datagram handle. */
	TY_DATAGRAM,                              /* Datagram: a received UDP payload + sender. */
	TY_OBJECT,
	TY_STRING   /* Immutable string. */
} TypeKind;
typedef struct TypeRef
{
	TypeKind kind;
	char class_name[64];
	struct TypeRef *elem;    /* TY_ARRAY element, or TY_MAP key; NULL otherwise. */
	struct TypeRef *elem2;   /* TY_MAP value; NULL otherwise. */
} TypeRef;

/* Width in bits of a scalar kind. Booleans report 1; objects/strings are
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
	case TY_ARRAY:
	case TY_MAP:
	case TY_GENERIC:
	case TY_ENTRY:
	case TY_CHANNEL:
	case TY_TIMER:
	case TY_LISTENER:
	case TY_SOCKET:
	case TY_UDPSOCKET:
	case TY_DATAGRAM:
	case TY_OBJECT:
	case TY_STRING:
		return 64;
	case TY_VOID:
		return 0;
	}

	return 0;
}

/* True for any integer kind (byte..ulong); excludes boolean. */
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

/* True for ARC-managed, heap, 8-byte-pointer kinds (objects and strings). Use
   this — not a bare `== TY_OBJECT` — wherever a retain/release decision is made. */
static inline int ty_is_managed(TypeKind k)
{
	return k == TY_OBJECT || k == TY_STRING || k == TY_ARRAY || k == TY_MAP || k == TY_GENERIC || k == TY_ENTRY || k == TY_CHANNEL || k == TY_TIMER
		   || k == TY_LISTENER || k == TY_SOCKET || k == TY_UDPSOCKET || k == TY_DATAGRAM;
}

/* Width rank for implicit widening: 8 < 16 < 32 < 64. Non-integers rank 0. */
static inline int ty_rank(TypeKind k)
{
	return ty_is_int(k) ? ty_bits(k) : 0;
}

typedef enum
{
	EX_INT, EX_BOOL, EX_FLOAT, EX_STR, EX_IDENT, EX_THIS, EX_NEW, EX_NEWARRAY,
	EX_NEWMAP, EX_NEWGEN, EX_NEWCHANNEL, EX_BINARY, EX_UNARY, EX_INCDEC, EX_CAST, EX_CALL, EX_METHOD_CALL, EX_FIELD, EX_INDEX
} ExprKind;

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
	Expr    *args[8];         /* Call / method call. */
	int      arg_count;
};

typedef enum { ST_VARDECL, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN, ST_EXPR, ST_FOREACH, ST_BREAK, ST_CONTINUE, ST_FOR, ST_SWITCH, ST_CASE, ST_DEFAULT, ST_THROW, ST_TRY, ST_CATCH, ST_SPAWN } StmtKind;

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
} Param;
typedef struct
{
	TypeRef ret_type;
	char name[64];
	Param   params[8];
	int param_count;
	Block  *body;
	int     frame_size;       /* Resolver. */
	int     obj_local_offsets[64];  /* Ownership pass: the stack offset of each object-typed local. */
	int     obj_local_count;        /* Number of entries in obj_local_offsets. */
	int     stack_alloc_bytes;      /* Escape pass: total frame bytes reserved for stack objects. */
} Func;

typedef struct
{
	TypeRef type;
	char name[64];
} Field;
typedef struct
{
	char  name[64];
	char parent_name[64];
	int has_parent;
	Field fields[32];
	int field_count;
	Func *methods[32];
	int method_count;
	Func *ctor;               /* The constructor (params + body), or NULL; ret_type is TY_VOID. */
} ClassDecl;

typedef struct
{
	ClassDecl *klass;         /* Non-NULL if this file declares a class. */
	Func      *funcs[8];      /* File-scope functions. */
	int        func_count;
} Unit;

void   ast_free_all(void);
Expr  *expr_new(ExprKind kind, int line);
TypeRef *typeref_box(TypeRef t);
Stmt  *stmt_new(StmtKind kind, int line);
Block *block_new(void);
void   block_push(Block *b, Stmt *s);
Func  *func_new(void);
ClassDecl *class_new(void);
Unit  *unit_new(void);

#endif
