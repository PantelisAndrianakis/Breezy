#include "symbols.h"
#include <stdio.h>
#include <string.h>

/* The LSP symbol index. The resolver has already stamped every expression with its
   result type and every named node with a column; this walks the
   resolved units and prints, for each named occurrence, its span + kind + type. The
   language server (src/lsp.c) loads this and answers hover from it. Definitions and
   the use->def link arrive in a later task; v1 is occurrence + type. */

/* ---- type rendering: a TypeRef -> a short display string ---- */

static void type_to_str(const TypeRef *t, char *out, int cap);

static void append(char *out, int cap, const char *s)
{
	int len = (int)strlen(out);
	int n = (int)strlen(s);
	if (len + n < cap)
	{
		memcpy(out + len, s, (size_t)n + 1);
	}
}

static void type_to_str(const TypeRef *t, char *out, int cap)
{
	switch (t->kind)
	{
	case TY_VOID:
		append(out, cap, "void");
		break;
	case TY_BOOL:
		append(out, cap, "bool");
		break;
	case TY_BYTE:
		append(out, cap, "byte");
		break;
	case TY_SHORT:
		append(out, cap, "short");
		break;
	case TY_INT:
		append(out, cap, "int");
		break;
	case TY_LONG:
		append(out, cap, "long");
		break;
	case TY_UBYTE:
		append(out, cap, "ubyte");
		break;
	case TY_USHORT:
		append(out, cap, "ushort");
		break;
	case TY_UINT:
		append(out, cap, "uint");
		break;
	case TY_ULONG:
		append(out, cap, "ulong");
		break;
	case TY_FLOAT:
		append(out, cap, "float");
		break;
	case TY_DOUBLE:
		append(out, cap, "double");
		break;
	case TY_STRING:
		append(out, cap, "string");
		break;
	case TY_NULL:
		append(out, cap, "null");
		break;
	case TY_ARRAY:
		if (t->elem)
		{
			type_to_str(t->elem, out, cap);
		}
		append(out, cap, "[]");
		break;
	case TY_MAP:
		append(out, cap, "map<");
		if (t->elem)
		{
			type_to_str(t->elem, out, cap);
		}
		append(out, cap, ", ");
		if (t->elem2)
		{
			type_to_str(t->elem2, out, cap);
		}
		append(out, cap, ">");
		break;
	case TY_CHANNEL:
		append(out, cap, "channel<");
		if (t->elem)
		{
			type_to_str(t->elem, out, cap);
		}
		append(out, cap, ">");
		break;
	case TY_GENERIC:
		append(out, cap, t->class_name);
		append(out, cap, "<");
		if (t->elem)
		{
			type_to_str(t->elem, out, cap);
		}
		append(out, cap, ">");
		break;
	case TY_OBJECT:
		append(out, cap, t->class_name[0] ? t->class_name : "object");
		break;
	case TY_FUNC:
		append(out, cap, "(");
		for (int i = 0; i < t->targ_count; i++)
		{
			if (i)
			{
				append(out, cap, ", ");
			}
			type_to_str(t->targs[i], out, cap);
		}
		append(out, cap, ")->");
		if (t->elem)
		{
			type_to_str(t->elem, out, cap);
		}
		break;
	default:
		/* The built-in managed handle kinds (Socket, PgConnection, XmlNode, ...) and
		   any kind without a bespoke spelling: fall back to the class name if set. */
		append(out, cap, t->class_name[0] ? t->class_name : "?");
		break;
	}
}

/* ---- the named-occurrence kind (syntactic) ---- */

static const char *expr_kind_name(ExprKind k)
{
	switch (k)
	{
	case EX_IDENT:
		return "variable";
	case EX_FIELD:
		return "field";
	case EX_METHOD_CALL:
		return "method";
	case EX_CALL:
		return "call";
	case EX_NEW:
		return "constructor";
	default:
		return "symbol";
	}
}

/* ---- JSON writing (escape backslashes -- Windows paths -- and quotes) ---- */

static void json_str(const char *s)
{
	putchar('"');
	for (const char *p = s; *p; p++)
	{
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\')
		{
			putchar('\\');
			putchar(c);
		}
		else if (c == '\n')
		{
			fputs("\\n", stdout);
		}
		else if (c == '\t')
		{
			fputs("\\t", stdout);
		}
		else if (c < 0x20)
		{
			printf("\\u%04x", c);
		}
		else
		{
			putchar(c);
		}
	}
	putchar('"');
}

/* ---- the walk ---- */

static const char *g_file;
static int g_first;
static Unit **g_units;
static int    g_nunits;

/* A user (non-prelude) unit has a real source path; the prelude's is "<source>". */
static int is_user_unit(const Unit *u)
{
	return u->file && u->file[0] && strcmp(u->file, "<source>") != 0;
}

/* Find a user class / free function by name and report the file it lives in. The
   declaration carries its name position (parser), so the caller has the def location.
   Linear scans -- fine at project scale, like the database drivers' column lookups. */
static ClassDecl *find_class(const char *name, const char **file)
{
	for (int i = 0; i < g_nunits; i++)
	{
		Unit *u = g_units[i];
		if (!is_user_unit(u))
		{
			continue;
		}
		for (int ci = 0; ci < u->class_count; ci++)
		{
			if (strcmp(u->klasses[ci]->name, name) == 0)
			{
				*file = u->file;
				return u->klasses[ci];
			}
		}
	}

	return NULL;
}

static Func *find_func(const char *name, const char **file)
{
	for (int i = 0; i < g_nunits; i++)
	{
		Unit *u = g_units[i];
		if (!is_user_unit(u))
		{
			continue;
		}
		for (int k = 0; k < u->func_count; k++)
		{
			if (strcmp(u->funcs[k]->name, name) == 0)
			{
				*file = u->file;
				return u->funcs[k];
			}
		}
	}

	return NULL;
}

/* Resolve the named occurrence to its declaration site (file + 1-based line/col), so
   the editor can jump there. Returns 1 when found. Locals/params are not resolved (no
   scope info here -- a later follow-up); cross-file classes, methods, fields, free
   functions, and constructors are. */
static int resolve_def(const Expr *e, const char **deffile, int *defline, int *defcol)
{
	const char *f = NULL;
	if (e->kind == EX_NEW || e->kind == EX_IDENT)
	{
		ClassDecl *c = find_class(e->name, &f);   /* `new Foo` / a bare class name. */
		if (c && c->name_line)
		{
			*deffile = f;
			*defline = c->name_line;
			*defcol = c->name_col;
			return 1;
		}
	}
	else if (e->kind == EX_CALL && !strchr(e->name, '.'))
	{
		Func *fn = find_func(e->name, &f);        /* A free function (not a namespace call). */
		if (fn && fn->name_line)
		{
			*deffile = f;
			*defline = fn->name_line;
			*defcol = fn->name_col;
			return 1;
		}
	}
	else if (e->kind == EX_METHOD_CALL && e->lhs && e->lhs->type.kind == TY_OBJECT)
	{
		ClassDecl *c = find_class(e->lhs->type.class_name, &f);
		for (int k = 0; c && k < c->method_count; k++)
		{
			if (strcmp(c->methods[k]->name, e->name) == 0 && c->methods[k]->name_line)
			{
				*deffile = f;
				*defline = c->methods[k]->name_line;
				*defcol = c->methods[k]->name_col;
				return 1;
			}
		}
	}
	else if (e->kind == EX_FIELD && e->lhs && e->lhs->type.kind == TY_OBJECT)
	{
		ClassDecl *c = find_class(e->lhs->type.class_name, &f);
		for (int k = 0; c && k < c->field_count; k++)
		{
			if (strcmp(c->fields[k].name, e->name) == 0 && c->fields[k].name_line)
			{
				*deffile = f;
				*defline = c->fields[k].name_line;
				*defcol = c->fields[k].name_col;
				return 1;
			}
		}
	}

	return 0;
}

static void emit_occurrence(const Expr *e)
{
	char ty[256];
	ty[0] = '\0';
	type_to_str(&e->type, ty, (int)sizeof(ty));

	int endcol = e->col + (int)strlen(e->name);   /* 1-based span [col, endCol). */

	if (!g_first)
	{
		putchar(',');
	}
	g_first = 0;

	fputs("{\"file\":", stdout);
	json_str(g_file ? g_file : "");
	printf(",\"line\":%d,\"col\":%d,\"endCol\":%d,\"name\":", e->line, e->col, endcol);
	json_str(e->name);
	printf(",\"kind\":\"%s\",\"type\":", expr_kind_name(e->kind));
	json_str(ty);

	const char *deffile = NULL;
	int defline = 0, defcol = 0;
	if (resolve_def(e, &deffile, &defline, &defcol))
	{
		fputs(",\"defFile\":", stdout);
		json_str(deffile);
		printf(",\"defLine\":%d,\"defCol\":%d", defline, defcol);
	}

	putchar('}');
}

static void walk_block(const Block *b);

static void walk_expr(const Expr *e)
{
	if (!e)
	{
		return;
	}
	if (e->col > 0 && e->name[0])
	{
		emit_occurrence(e);
	}

	walk_expr(e->lhs);
	walk_expr(e->rhs);
	for (int i = 0; i < e->arg_count; i++)
	{
		walk_expr(e->args[i]);
	}
}

static void walk_stmt(const Stmt *s)
{
	if (!s)
	{
		return;
	}

	walk_expr(s->decl_init);
	walk_expr(s->target);
	walk_expr(s->value);
	walk_expr(s->cond);
	walk_expr(s->ret_val);
	walk_expr(s->expr);
	walk_stmt(s->for_init);
	walk_stmt(s->for_post);
	walk_block(s->then_blk);
	walk_block(s->else_blk);

	for (int i = 0; i < s->sel_arm_count; i++)
	{
		walk_expr(s->sel_arms[i].chan);
		walk_expr(s->sel_arms[i].send_val);
		walk_block(s->sel_arms[i].body);
	}
}

static void walk_block(const Block *b)
{
	if (!b)
	{
		return;
	}
	for (int i = 0; i < b->count; i++)
	{
		walk_stmt(b->stmts[i]);
	}
}

static void walk_func(const Func *f)
{
	if (f && !f->is_extern)
	{
		walk_block(f->body);
	}
}

void symbols_emit(Unit **units, int unit_count)
{
	g_units = units;
	g_nunits = unit_count;
	g_first = 1;
	fputs("{\"symbols\":[", stdout);

	for (int i = 0; i < unit_count; i++)
	{
		Unit *u = units[i];
		/* Skip the prelude / desktop-prelude units: they have no real source path
		   (the lexer's "<source>" default), and their symbols are not in the user's
		   files, so they would only pollute the editor's index. */
		if (!u->file || u->file[0] == '\0' || strcmp(u->file, "<source>") == 0)
		{
			continue;
		}
		g_file = u->file;
		for (int k = 0; k < u->func_count; k++)
		{
			walk_func(u->funcs[k]);
		}
		for (int ci = 0; ci < u->class_count; ci++)
		{
			ClassDecl *d = u->klasses[ci];
			for (int k = 0; k < d->method_count; k++)
			{
				walk_func(d->methods[k]);
			}
			for (int k = 0; k < d->ctor_count; k++)
			{
				walk_func(d->ctors[k]);
			}
		}
	}

	fputs("]}\n", stdout);
}
