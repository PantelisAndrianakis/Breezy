#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===== Pattern AST ===== */
enum { N_EMPTY, N_LIT, N_ANY, N_CLASS, N_CAT, N_ALT, N_STAR, N_PLUS, N_QUEST, N_REPEAT, N_BOL, N_EOL };

typedef struct Node Node;
struct Node
{
	int type;
	unsigned char ch;        /* N_LIT. */
	unsigned char set[32];   /* N_CLASS: 256-bit membership bitmap. */
	int neg;                 /* N_CLASS: negated. */
	int min, max;            /* N_REPEAT: max < 0 means unbounded. */
	Node *a, *b;
};

typedef struct
{
	const char *p;
	const char *end;
	int ok;
} PS;

static Node *node_new(int type)
{
	Node *n = calloc(1, sizeof(Node));
	n->type = type;
	return n;
}

static void node_free(Node *n)
{
	if (!n)
	{
		return;
	}

	node_free(n->a);
	node_free(n->b);
	free(n);
}

static void set_bit(unsigned char *set, unsigned char c)
{
	set[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static int ps_peek(PS *s)
{
	return s->p < s->end ? (unsigned char)*s->p : -1;
}

static int ps_next(PS *s)
{
	return s->p < s->end ? (unsigned char)*s->p++ : -1;
}

static void add_digit(unsigned char *s)
{
	for (int c = '0'; c <= '9'; c++)
	{
		set_bit(s, (unsigned char)c);
	}
}

static void add_word(unsigned char *s)
{
	add_digit(s);
	for (int c = 'a'; c <= 'z'; c++)
	{
		set_bit(s, (unsigned char)c);
	}

	for (int c = 'A'; c <= 'Z'; c++)
	{
		set_bit(s, (unsigned char)c);
	}

	set_bit(s, '_');
}

static void add_space(unsigned char *s)
{
	set_bit(s, ' ');
	set_bit(s, '\t');
	set_bit(s, '\n');
	set_bit(s, '\r');
	set_bit(s, '\f');
	set_bit(s, '\v');
}

/* Fill a class set for a \d \D \w \W \s \S escape; sets *neg for the uppercase forms. */
static void fill_class_escape(int e, unsigned char *set, int *neg)
{
	switch (e)
	{
	case 'd':
		add_digit(set);
		break;
	case 'D':
		add_digit(set);
		*neg = 1;
		break;
	case 'w':
		add_word(set);
		break;
	case 'W':
		add_word(set);
		*neg = 1;
		break;
	case 's':
		add_space(set);
		break;
	case 'S':
		add_space(set);
		*neg = 1;
		break;
	}
}

static Node *parse_alt(PS *s);

/* The character after a backslash, as an atom (class escape or escaped literal). */
static Node *parse_escape(PS *s)
{
	int e = ps_next(s);
	if (e < 0)
	{
		s->ok = 0;
		return node_new(N_EMPTY);
	}

	if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S')
	{
		Node *n = node_new(N_CLASS);
		fill_class_escape(e, n->set, &n->neg);
		return n;
	}

	Node *n = node_new(N_LIT);
	n->ch = (unsigned char)(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
	return n;
}

static Node *parse_class(PS *s)
{
	ps_next(s);                          /* '['. */
	Node *n = node_new(N_CLASS);
	if (ps_peek(s) == '^')
	{
		n->neg = 1;
		ps_next(s);
	}

	int first = 1;
	while (ps_peek(s) >= 0 && (ps_peek(s) != ']' || first))
	{
		first = 0;
		int c = ps_next(s);
		if (c == '\\')
		{
			int e = ps_next(s);
			if (e == 'd' || e == 'w' || e == 's')
			{
				int dummy = 0;
				fill_class_escape(e, n->set, &dummy);
				continue;
			}

			c = (e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
		}

		if (ps_peek(s) == '-' && s->p + 1 < s->end && s->p[1] != ']')
		{
			ps_next(s);                  /* '-'. */
			int hi = ps_next(s);
			if (hi == '\\')
			{
				int e2 = ps_next(s);
				hi = (e2 == 'n' ? '\n' : e2 == 't' ? '\t' : e2 == 'r' ? '\r' : e2);
			}

			for (int x = c; x <= hi; x++)
			{
				set_bit(n->set, (unsigned char)x);
			}
		}
		else
		{
			set_bit(n->set, (unsigned char)c);
		}
	}

	if (ps_peek(s) == ']')
	{
		ps_next(s);
	}
	else
	{
		s->ok = 0;
	}

	return n;
}

static Node *parse_atom(PS *s)
{
	int c = ps_peek(s);
	if (c == '(')
	{
		ps_next(s);
		Node *n = parse_alt(s);
		if (ps_peek(s) == ')')
		{
			ps_next(s);
		}
		else
		{
			s->ok = 0;
		}

		return n;
	}

	if (c == '[')
	{
		return parse_class(s);
	}

	if (c == '\\')
	{
		ps_next(s);
		return parse_escape(s);
	}

	if (c == '.')
	{
		ps_next(s);
		return node_new(N_ANY);
	}

	if (c == '^')
	{
		ps_next(s);
		return node_new(N_BOL);
	}

	if (c == '$')
	{
		ps_next(s);
		return node_new(N_EOL);
	}

	ps_next(s);                          /* A literal (including a stray * + ? { treated literally). */
	Node *n = node_new(N_LIT);
	n->ch = (unsigned char)c;
	return n;
}

static Node *parse_repeat(PS *s)
{
	Node *a = parse_atom(s);
	for (;;)
	{
		int c = ps_peek(s);
		if (c == '*')
		{
			ps_next(s);
			Node *n = node_new(N_STAR);
			n->a = a;
			a = n;
		}
		else if (c == '+')
		{
			ps_next(s);
			Node *n = node_new(N_PLUS);
			n->a = a;
			a = n;
		}
		else if (c == '?')
		{
			ps_next(s);
			Node *n = node_new(N_QUEST);
			n->a = a;
			a = n;
		}
		else if (c == '{')
		{
			const char *save = s->p;
			ps_next(s);                  /* '{'. */
			int mn = 0, mx = -1, got = 0;
			while (ps_peek(s) >= '0' && ps_peek(s) <= '9')
			{
				mn = mn * 10 + (ps_next(s) - '0');
				got = 1;
			}

			if (!got)
			{
				s->p = save;             /* Not a quantifier; leave '{' to be a literal. */
				break;
			}

			if (ps_peek(s) == ',')
			{
				ps_next(s);
				if (ps_peek(s) >= '0' && ps_peek(s) <= '9')
				{
					mx = 0;
					while (ps_peek(s) >= '0' && ps_peek(s) <= '9')
					{
						mx = mx * 10 + (ps_next(s) - '0');
					}
				}
			}
			else
			{
				mx = mn;
			}

			if (ps_peek(s) == '}')
			{
				ps_next(s);
			}
			else
			{
				s->p = save;
				break;
			}

			if (mn > 1000 || mx > 1000)
			{
				s->ok = 0;
				break;
			}

			Node *n = node_new(N_REPEAT);
			n->a = a;
			n->min = mn;
			n->max = mx;
			a = n;
		}
		else
		{
			break;
		}
	}

	return a;
}

static Node *parse_concat(PS *s)
{
	Node *n = NULL;
	while (ps_peek(s) >= 0 && ps_peek(s) != '|' && ps_peek(s) != ')')
	{
		Node *r = parse_repeat(s);
		if (!n)
		{
			n = r;
		}
		else
		{
			Node *c = node_new(N_CAT);
			c->a = n;
			c->b = r;
			n = c;
		}
	}

	return n ? n : node_new(N_EMPTY);
}

static Node *parse_alt(PS *s)
{
	Node *n = parse_concat(s);
	while (ps_peek(s) == '|')
	{
		ps_next(s);
		Node *r = parse_concat(s);
		Node *al = node_new(N_ALT);
		al->a = n;
		al->b = r;
		n = al;
	}

	return n;
}

/* ===== Compiled program ===== */
enum { I_CHAR, I_ANY, I_CLASS, I_MATCH, I_JMP, I_SPLIT, I_BOL, I_EOL, I_SAVE };

typedef struct
{
	int op;
	unsigned char ch;
	int x, y;
	unsigned char set[32];
	int neg;
} Inst;

typedef struct
{
	Inst *in;
	int n, cap;
} Prog;

static int emit(Prog *p, int op)
{
	if (p->n == p->cap)
	{
		p->cap = p->cap ? p->cap * 2 : 64;
		p->in = realloc(p->in, (size_t)p->cap * sizeof(Inst));
	}

	memset(&p->in[p->n], 0, sizeof(Inst));
	p->in[p->n].op = op;
	return p->n++;
}

static void emit_node(Prog *p, Node *n)
{
	switch (n->type)
	{
	case N_EMPTY:
		break;
	case N_LIT:
	{
		int i = emit(p, I_CHAR);
		p->in[i].ch = n->ch;
		break;
	}
	case N_ANY:
		emit(p, I_ANY);
		break;
	case N_CLASS:
	{
		int i = emit(p, I_CLASS);
		memcpy(p->in[i].set, n->set, 32);
		p->in[i].neg = n->neg;
		break;
	}
	case N_BOL:
		emit(p, I_BOL);
		break;
	case N_EOL:
		emit(p, I_EOL);
		break;
	case N_CAT:
		emit_node(p, n->a);
		emit_node(p, n->b);
		break;
	case N_ALT:
	{
		int sp = emit(p, I_SPLIT);
		p->in[sp].x = p->n;
		emit_node(p, n->a);
		int jm = emit(p, I_JMP);
		p->in[sp].y = p->n;
		emit_node(p, n->b);
		p->in[jm].x = p->n;
		break;
	}
	case N_STAR:
	{
		int sp = emit(p, I_SPLIT);
		p->in[sp].x = p->n;
		emit_node(p, n->a);
		int jm = emit(p, I_JMP);
		p->in[jm].x = sp;
		p->in[sp].y = p->n;
		break;
	}
	case N_PLUS:
	{
		int l1 = p->n;
		emit_node(p, n->a);
		int sp = emit(p, I_SPLIT);
		p->in[sp].x = l1;
		p->in[sp].y = p->n;
		break;
	}
	case N_QUEST:
	{
		int sp = emit(p, I_SPLIT);
		p->in[sp].x = p->n;
		emit_node(p, n->a);
		p->in[sp].y = p->n;
		break;
	}
	case N_REPEAT:
	{
		for (int i = 0; i < n->min; i++)
		{
			emit_node(p, n->a);
		}

		if (n->max < 0)
		{
			int sp = emit(p, I_SPLIT);
			p->in[sp].x = p->n;
			emit_node(p, n->a);
			int jm = emit(p, I_JMP);
			p->in[jm].x = sp;
			p->in[sp].y = p->n;
		}
		else
		{
			for (int i = 0; i < n->max - n->min; i++)
			{
				int sp = emit(p, I_SPLIT);
				p->in[sp].x = p->n;
				emit_node(p, n->a);
				p->in[sp].y = p->n;
			}
		}

		break;
	}
	}
}

/* Compile pattern -> program. unanchored prepends a non-greedy .*? search loop;
   pc 0 is always the start. An empty program (n == 0) never matches. */
static Prog *compile(const char *pat, int patlen, int unanchored)
{
	PS s;
	s.p = pat;
	s.end = pat + patlen;
	s.ok = 1;
	Node *root = parse_alt(&s);
	if (s.p != s.end)
	{
		s.ok = 0;                        /* Trailing garbage, e.g. an unmatched ')'. */
	}

	Prog *p = calloc(1, sizeof(Prog));
	if (!s.ok)
	{
		node_free(root);
		return p;                        /* n == 0 -> never matches. */
	}

	if (unanchored)
	{
		int sp = emit(p, I_SPLIT);       /* pc 0. */
		p->in[sp].x = p->n;              /* Prefer to start matching here (leftmost). */
		emit(p, I_SAVE);                 /* Record the match start. */
		emit_node(p, root);
		emit(p, I_MATCH);
		p->in[sp].y = p->n;              /* Else consume one char and retry. */
		emit(p, I_ANY);
		int jm = emit(p, I_JMP);
		p->in[jm].x = sp;
	}
	else
	{
		emit(p, I_SAVE);
		emit_node(p, root);
		emit(p, I_MATCH);
	}

	node_free(root);
	return p;
}

/* ===== Pike VM ===== */
typedef struct
{
	int pc;
	int start;
} Th;

static int cls_match(Inst *in, unsigned char c)
{
	int b = (in->set[c >> 3] >> (c & 7)) & 1;
	return in->neg ? !b : b;
}

static void addthread(Prog *p, Th *l, int *ln, int *seen, int gen,
					  int pc, int start, int sp, int len)
{
	if (seen[pc] == gen)
	{
		return;
	}

	seen[pc] = gen;
	Inst *in = &p->in[pc];
	switch (in->op)
	{
	case I_JMP:
		addthread(p, l, ln, seen, gen, in->x, start, sp, len);
		break;
	case I_SPLIT:
		addthread(p, l, ln, seen, gen, in->x, start, sp, len);
		addthread(p, l, ln, seen, gen, in->y, start, sp, len);
		break;
	case I_SAVE:
		addthread(p, l, ln, seen, gen, pc + 1, sp, sp, len);   /* start = sp. */
		break;
	case I_BOL:
		if (sp == 0)
		{
			addthread(p, l, ln, seen, gen, pc + 1, start, sp, len);
		}

		break;
	case I_EOL:
		if (sp == len)
		{
			addthread(p, l, ln, seen, gen, pc + 1, start, sp, len);
		}

		break;
	default:                             /* I_CHAR / I_ANY / I_CLASS / I_MATCH. */
		l[*ln].pc = pc;
		l[*ln].start = start;
		(*ln)++;
		break;
	}
}

/* Run the program over text[0,len). anchored: accept only at end of input.
   Returns 1 and (optionally) the leftmost match span on success. */
static int run(Prog *p, const char *text, int len, int anchored, int *os, int *oe)
{
	if (p->n == 0)
	{
		return 0;
	}

	Th *cl = malloc(sizeof(Th) * (size_t)p->n);
	Th *nl = malloc(sizeof(Th) * (size_t)p->n);
	int *seen = malloc(sizeof(int) * (size_t)p->n);
	for (int i = 0; i < p->n; i++)
	{
		seen[i] = -1;
	}

	int gen = 0, cln = 0, matched = 0, ms = -1, me = -1;
	gen++;
	addthread(p, cl, &cln, seen, gen, 0, 0, 0, len);
	for (int sp = 0; ; sp++)
	{
		unsigned char c = (sp < len) ? (unsigned char)text[sp] : 0;
		int nln = 0;
		gen++;
		for (int i = 0; i < cln; i++)
		{
			Inst *in = &p->in[cl[i].pc];
			int st = cl[i].start;
			if (in->op == I_CHAR)
			{
				if (sp < len && c == in->ch)
				{
					addthread(p, nl, &nln, seen, gen, cl[i].pc + 1, st, sp + 1, len);
				}
			}
			else if (in->op == I_ANY)
			{
				if (sp < len)
				{
					addthread(p, nl, &nln, seen, gen, cl[i].pc + 1, st, sp + 1, len);
				}
			}
			else if (in->op == I_CLASS)
			{
				if (sp < len && cls_match(in, c))
				{
					addthread(p, nl, &nln, seen, gen, cl[i].pc + 1, st, sp + 1, len);
				}
			}
			else if (in->op == I_MATCH)
			{
				if (!anchored || sp == len)
				{
					matched = 1;
					ms = st;
					me = sp;
					break;               /* Accepted: drop lower-priority threads (leftmost-first). */
				}

				/* Anchored but not at end: this thread fails; lower-priority threads live on. */
			}
		}

		Th *tmp = cl;
		cl = nl;
		nl = tmp;
		cln = nln;
		if (sp >= len)
		{
			break;
		}
	}

	free(cl);
	free(nl);
	free(seen);
	if (matched)
	{
		if (os)
		{
			*os = ms;
		}

		if (oe)
		{
			*oe = me;
		}

		return 1;
	}

	return 0;
}

int64_t bzy_regex_matches(void *pat, void *text)
{
	Prog *p = compile(bzy_str_data(pat), (int)bzy_str_len(pat), 0);
	int r = run(p, bzy_str_data(text), (int)bzy_str_len(text), 1, NULL, NULL);
	free(p->in);
	free(p);
	return r;
}

int64_t bzy_regex_test(void *pat, void *text)
{
	Prog *p = compile(bzy_str_data(pat), (int)bzy_str_len(pat), 1);
	int r = run(p, bzy_str_data(text), (int)bzy_str_len(text), 0, NULL, NULL);
	free(p->in);
	free(p);
	return r;
}
