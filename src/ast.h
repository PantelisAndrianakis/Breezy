#ifndef AST_H
#define AST_H

typedef enum { TY_INT, TY_VOID, TY_OBJECT } TypeKind;
typedef struct
{
	TypeKind kind;
	char class_name[64];
} TypeRef;

typedef enum
{
	EX_INT, EX_IDENT, EX_THIS, EX_NEW,
	EX_BINARY, EX_UNARY, EX_CALL, EX_METHOD_CALL, EX_FIELD
} ExprKind;

typedef struct Expr Expr;
struct Expr
{
	ExprKind kind;
	int      line;
	TypeRef  type;            /* resolver: result type */
	int      anno_int;        /* resolver: stack offset / field offset / vtable slot */
	char     anno_str[64];    /* resolver: static class for dispatch */
	long     int_val;         /* EX_INT */
	char     name[64];        /* EX_IDENT/NEW/CALL/METHOD_CALL/FIELD */
	int      op;              /* EX_BINARY/EX_UNARY: a TokenType */
	Expr    *lhs;             /* binary left / unary operand / method-call|field receiver */
	Expr    *rhs;             /* binary right */
	Expr    *args[8];         /* call / method call */
	int      arg_count;
};

typedef enum { ST_VARDECL, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN, ST_EXPR } StmtKind;

typedef struct Stmt Stmt;
typedef struct Block Block;
struct Stmt
{
	StmtKind kind;
	int      line;
	TypeRef  decl_type;       /* ST_VARDECL */
	char     decl_name[64];
	Expr    *decl_init;       /* may be NULL */
	int      decl_offset;     /* resolver: stack slot for this local */
	Expr    *target;          /* ST_ASSIGN lvalue (EX_IDENT|EX_FIELD) */
	Expr    *value;           /* ST_ASSIGN rhs */
	Expr    *cond;            /* ST_IF / ST_WHILE */
	Block   *then_blk;        /* then-branch / while-body */
	Block   *else_blk;        /* else-branch (NULL if none) */
	Expr    *ret_val;         /* ST_RETURN (may be NULL) */
	Expr    *expr;            /* ST_EXPR */
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
	int     frame_size;       /* resolver */
	int     obj_local_offsets[64];  /* Ownership pass: the stack offset of each object-typed local. */
	int     obj_local_count;        /* Number of entries in obj_local_offsets. */
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
} ClassDecl;

typedef struct
{
	ClassDecl *klass;         /* non-NULL if this file declares a class */
	Func      *funcs[8];      /* file-scope functions */
	int        func_count;
} Unit;

void   ast_free_all(void);
Expr  *expr_new(ExprKind kind, int line);
Stmt  *stmt_new(StmtKind kind, int line);
Block *block_new(void);
void   block_push(Block *b, Stmt *s);
Func  *func_new(void);
ClassDecl *class_new(void);
Unit  *unit_new(void);

#endif
