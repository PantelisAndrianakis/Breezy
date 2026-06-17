/* TreeMap<K,V> / TreeSet<T>: a B-tree (min degree BT_B). Two storage modes keep
   the common case fast and the object case cycle-safe:

   - PRIMITIVE mode (int/long/double key, primitive/absent value): nodes are raw
     allocations, keys/values are bit patterns, the tree is freed by a recursive
     walk. No ARC, no cycle tracing -- primitives cannot form cycles. This is the
     hot path (the perf gate runs here).
   - MANAGED mode (object/string key, or managed value): nodes are managed objects
     whose type descriptor declares their key/value/child-pointer slots, so ARC
     and the cycle collector reach every key/value/subtree exactly as the vector
     reaches its elements. Releasing the tree handle releases the root, which
     recursively releases the whole tree; an object cycle through the map is
     reclaimed. Keys/values are ARC-retained on insert (the slot owns a ref) and
     released automatically when their node is freed.

   Ordering: bzy_order_cmp_for by key kind for primitives/strings; bzy_obj_compare
   at the compareTo vtable slot for object keys. TreeSet is a TreeMap with no value
   column (has_values = 0). Own TU: a program using neither container drops it.

   Node layout (header present so a managed node IS the object; raw nodes leave the
   header unused): vtable | rc | gcinfo | n | leaf | key[BT_MAX] | val[BT_MAX] |
   kid[BT_MAX+1]. Handle layout (object_size 80): vtable | rc | gcinfo | 24 root |
   32 count | 40 kkind | 48 has_values | 56 vman | 64 obj_slot | 72 cmp. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#define BT_B   16                 /* Min degree; a node holds [BT_B-1 .. 2*BT_B-1] keys. */
#define BT_MAX (2 * BT_B - 1)

typedef struct BTNode
{
	void   *vtable;               /* +0  (managed mode only). */
	int64_t rc;                   /* +8 */
	int64_t gcinfo;               /* +16 */
	int64_t n;                    /* +24 key count. */
	int64_t leaf;                 /* +32 */
	int64_t key[BT_MAX];          /* +40 */
	int64_t val[BT_MAX];
	int64_t kid[BT_MAX + 1];      /* Child node pointers (managed-node objects in managed mode). */
} BTNode;

#define KEY_OFF ((int64_t)offsetof(BTNode, key))
#define VAL_OFF ((int64_t)offsetof(BTNode, val))
#define KID_OFF ((int64_t)offsetof(BTNode, kid))

/* Handle accessors. */
static int64_t  *HROOT(void *o)  { return (int64_t*)((char*)o + 24); }
static int64_t  *HCOUNT(void *o) { return (int64_t*)((char*)o + 32); }
static int64_t  *HKIND(void *o)  { return (int64_t*)((char*)o + 40); }
static int64_t  *HHV(void *o)    { return (int64_t*)((char*)o + 48); }
static int64_t  *HVMAN(void *o)  { return (int64_t*)((char*)o + 56); }
static int64_t  *HSLOT(void *o)  { return (int64_t*)((char*)o + 64); }
static bzy_cmp_fn *HCMP(void *o) { return (bzy_cmp_fn*)((char*)o + 72); }

static BTNode *bt_root(void *o)  { return (BTNode*)*HROOT(o); }
static int bt_kman(void *o)      { int64_t k = *HKIND(o); return k == 3 || k == 4; }
static int bt_managed(void *o)   { return bt_kman(o) || (int)*HVMAN(o); }

static int bt_shared(void *o)
{
	return (*(int64_t*)((char*)o + 16) & (1ll << 3)) != 0;
}

static int bt_cmp(void *o, int64_t a, int64_t b)
{
	int64_t slot = *HSLOT(o);
	return slot >= 0 ? bzy_obj_compare((void*)a, (void*)b, slot) : (*HCMP(o))(a, b);
}

/* ---- managed-node descriptors (lazy, one per (kman,vman) combo) ---- */
#define TI_LEN (2 + (BT_MAX + 1) + 2 * BT_MAX)
static int64_t g_node_ti[4][TI_LEN];
static int64_t g_node_vt[4][2];
static int     g_node_ti_built[4];

static void *node_vtable(int kman, int vman)
{
	int combo = (kman ? 1 : 0) | (vman ? 2 : 0);
	if (!g_node_ti_built[combo])
	{
		int64_t *ti = g_node_ti[combo];
		int cnt = 0;
		ti[0] = 0;   /* No finalizer; ARC releases declared children. */
		for (int j = 0; j <= BT_MAX; j++)
		{
			ti[2 + cnt++] = KID_OFF + (int64_t)j * 8;   /* Child node pointers (always, in managed mode). */
		}

		if (kman)
		{
			for (int i = 0; i < BT_MAX; i++)
			{
				ti[2 + cnt++] = KEY_OFF + (int64_t)i * 8;
			}
		}

		if (vman)
		{
			for (int i = 0; i < BT_MAX; i++)
			{
				ti[2 + cnt++] = VAL_OFF + (int64_t)i * 8;
			}
		}

		ti[1] = cnt;
		g_node_vt[combo][0] = (int64_t)&ti[0];
		g_node_ti_built[combo] = 1;
	}

	return &g_node_vt[combo][1];
}

static BTNode *bt_node_new(void *o, int leaf)
{
	BTNode *x;
	if (bt_managed(o))
	{
		x = (BTNode*)bzy_alloc(sizeof(BTNode));   /* Zeroed; rc=1; gcinfo set by alloc. */
		x->vtable = node_vtable(bt_kman(o), (int)*HVMAN(o));
	}
	else
	{
		x = (BTNode*)calloc(1, sizeof(BTNode));
	}

	x->leaf = leaf;
	return x;
}

/* ---- handle descriptors ---- */
static void bt_free_raw(void *o);   /* Forward. */

static int64_t g_h_ti_managed[3] = { 0, 1, 24 };          /* One child: root@24. */
static int64_t g_h_ti_raw[2];                              /* { finalizer, 0 }. */
static int64_t g_h_vt_managed[2];
static int64_t g_h_vt_raw[2];

static void *handle_vtable_managed(void)
{
	g_h_vt_managed[0] = (int64_t)&g_h_ti_managed[0];
	return &g_h_vt_managed[1];
}

static void *handle_vtable_raw(void)
{
	g_h_ti_raw[0] = (int64_t)(void*)bt_free_raw;
	g_h_ti_raw[1] = 0;
	g_h_vt_raw[0] = (int64_t)&g_h_ti_raw[0];
	return &g_h_vt_raw[1];
}

static void bt_free_raw_node(BTNode *x)
{
	if (!x)
	{
		return;
	}

	if (!x->leaf)
	{
		for (int i = 0; i <= x->n; i++)
		{
			bt_free_raw_node((BTNode*)x->kid[i]);
		}
	}

	free(x);
}

static void bt_free_raw(void *o)
{
	bt_free_raw_node(bt_root(o));
}

void *bzy_btree_new(int64_t kkind, int64_t has_values, int64_t vman, int64_t obj_slot)
{
	int kman = (kkind == 3 || kkind == 4);
	int managed = kman || (int)vman;

	void *o = bzy_alloc(80);
	*(void**)o = managed ? handle_vtable_managed() : handle_vtable_raw();
	*HKIND(o) = kkind;
	*HHV(o) = has_values;
	*HVMAN(o) = vman;
	*HSLOT(o) = obj_slot;
	*HCMP(o) = obj_slot >= 0 ? (bzy_cmp_fn)0 : bzy_order_cmp_for(kkind);
	*HCOUNT(o) = 0;
	*HROOT(o) = (int64_t)(intptr_t)bt_node_new(o, 1);   /* Empty root leaf (owned by the handle). */
	return o;
}

/* Split full child c->kid[i] of the non-full node c (both already allocated). */
static void bt_split(void *o, BTNode *c, int i, int has_values)
{
	BTNode *y = (BTNode*)c->kid[i];
	BTNode *z = bt_node_new(o, (int)y->leaf);
	z->n = BT_B - 1;
	for (int j = 0; j < BT_B - 1; j++)
	{
		z->key[j] = y->key[j + BT_B];
		y->key[j + BT_B] = 0;
		if (has_values)
		{
			z->val[j] = y->val[j + BT_B];
			y->val[j + BT_B] = 0;
		}
	}

	if (!y->leaf)
	{
		for (int j = 0; j < BT_B; j++)
		{
			z->kid[j] = y->kid[j + BT_B];
			y->kid[j + BT_B] = 0;
		}
	}

	y->n = BT_B - 1;
	for (int j = c->n; j >= i + 1; j--)
	{
		c->kid[j + 1] = c->kid[j];
	}

	c->kid[i + 1] = (int64_t)(intptr_t)z;
	for (int j = (int)c->n - 1; j >= i; j--)
	{
		c->key[j + 1] = c->key[j];
		if (has_values)
		{
			c->val[j + 1] = c->val[j];
		}
	}

	c->key[i] = y->key[BT_B - 1];
	y->key[BT_B - 1] = 0;
	if (has_values)
	{
		c->val[i] = y->val[BT_B - 1];
		y->val[BT_B - 1] = 0;
	}

	c->n++;
}

/* Overwrite the value at an existing key slot (managed value: retain new, release old). */
static void bt_set_val(void *o, BTNode *x, int i, int64_t v)
{
	if (!*HHV(o))
	{
		return;
	}

	if ((int)*HVMAN(o))
	{
		bzy_retain((void*)v);
		bzy_release((void*)x->val[i]);
	}

	x->val[i] = v;
}

/* Insert (k,v) into the non-full subtree rooted at x. Returns 1 if a new key was
   added (vs an overwrite of an existing key). */
static int bt_insert_nonfull(void *o, BTNode *x, int64_t k, int64_t v)
{
	int has_values = (int)*HHV(o);
	int kman = bt_kman(o);
	int i = (int)x->n - 1;
	while (i >= 0 && bt_cmp(o, k, x->key[i]) < 0)
	{
		i--;
	}

	if (i >= 0 && bt_cmp(o, k, x->key[i]) == 0)
	{
		bt_set_val(o, x, i, v);
		return 0;
	}

	if (x->leaf)
	{
		for (int j = (int)x->n - 1; j > i; j--)
		{
			x->key[j + 1] = x->key[j];
			if (has_values)
			{
				x->val[j + 1] = x->val[j];
			}
		}

		if (kman)
		{
			bzy_retain((void*)k);
		}

		if (has_values && (int)*HVMAN(o))
		{
			bzy_retain((void*)v);
		}

		x->key[i + 1] = k;
		if (has_values)
		{
			x->val[i + 1] = v;
		}

		x->n++;
		return 1;
	}

	i++;
	BTNode *child = (BTNode*)x->kid[i];
	if (child->n == BT_MAX)
	{
		bt_split(o, x, i, has_values);
		int c = bt_cmp(o, k, x->key[i]);
		if (c == 0)
		{
			bt_set_val(o, x, i, v);
			return 0;
		}

		if (c > 0)
		{
			i++;
		}
	}

	return bt_insert_nonfull(o, (BTNode*)x->kid[i], k, v);
}

static void bt_put_impl(void *o, int64_t k, int64_t v)
{
	BTNode *r = bt_root(o);
	if (r->n == BT_MAX)
	{
		BTNode *s = bt_node_new(o, 0);
		s->kid[0] = (int64_t)(intptr_t)r;      /* Transfer the old root under the new root. */
		*HROOT(o) = (int64_t)(intptr_t)s;
		bt_split(o, s, 0, (int)*HHV(o));
		*HCOUNT(o) += bt_insert_nonfull(o, s, k, v);
	}
	else
	{
		*HCOUNT(o) += bt_insert_nonfull(o, r, k, v);
	}
}

void bzy_btree_put(void *o, int64_t k, int64_t v)
{
	if (bt_shared(o))
	{
		if (bt_kman(o))
		{
			bzy_share_crosscore((void*)k);
		}

		if (*HHV(o) && (int)*HVMAN(o))
		{
			bzy_share_crosscore((void*)v);
		}

		bzy_shared_lock(o);
		bt_put_impl(o, k, v);
		bzy_shared_unlock(o);
		return;
	}

	bt_put_impl(o, k, v);
}

/* Locate the node + index holding k, or NULL. */
static BTNode *bt_find(void *o, int64_t k, int *idx)
{
	BTNode *x = bt_root(o);
	while (x)
	{
		int i = 0;
		while (i < x->n && bt_cmp(o, k, x->key[i]) > 0)
		{
			i++;
		}

		if (i < x->n && bt_cmp(o, k, x->key[i]) == 0)
		{
			*idx = i;
			return x;
		}

		if (x->leaf)
		{
			return NULL;
		}

		x = (BTNode*)x->kid[i];
	}

	return NULL;
}

int64_t bzy_btree_get(void *o, int64_t k)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		int i;
		BTNode *x = bt_find(o, k, &i);
		int64_t r = 0;
		if (x)
		{
			r = *HHV(o) ? x->val[i] : 1;
			if (*HHV(o) && (int)*HVMAN(o))
			{
				bzy_retain((void*)r);
			}
		}

		bzy_shared_unlock(o);
		return r;
	}

	int i;
	BTNode *x = bt_find(o, k, &i);
	if (!x)
	{
		return 0;
	}

	int64_t r = *HHV(o) ? x->val[i] : 1;
	if (*HHV(o) && (int)*HVMAN(o))
	{
		bzy_retain((void*)r);
	}

	return r;
}

int64_t bzy_btree_has(void *o, int64_t k)
{
	int64_t r;
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		int i;
		r = bt_find(o, k, &i) != NULL;
		bzy_shared_unlock(o);
		return r;
	}

	int i;
	return bt_find(o, k, &i) != NULL;
}

int64_t bzy_btree_size(void *o)
{
	return *HCOUNT(o);
}

/* first / last key (retained when managed). */
static int64_t bt_extreme(void *o, int last)
{
	BTNode *x = bt_root(o);
	if (x->n == 0)
	{
		return 0;
	}

	while (!x->leaf)
	{
		x = (BTNode*)x->kid[last ? x->n : 0];
	}

	int64_t r = x->key[last ? x->n - 1 : 0];
	if (bt_kman(o))
	{
		bzy_retain((void*)r);
	}

	return r;
}

int64_t bzy_btree_first(void *o)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = bt_extreme(o, 0);
		bzy_shared_unlock(o);
		return r;
	}

	return bt_extreme(o, 0);
}

int64_t bzy_btree_last(void *o)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = bt_extreme(o, 1);
		bzy_shared_unlock(o);
		return r;
	}

	return bt_extreme(o, 1);
}

/* floor (greatest key <= k) / ceiling (least key >= k); 0 if none. */
static int64_t bt_bound(void *o, int64_t k, int want_floor, int *found)
{
	BTNode *x = bt_root(o);
	int64_t best = 0;
	*found = 0;
	while (x)
	{
		int i = 0;
		while (i < x->n && bt_cmp(o, k, x->key[i]) > 0)
		{
			i++;
		}

		if (i < x->n && bt_cmp(o, k, x->key[i]) == 0)
		{
			*found = 1;
			return x->key[i];
		}

		/* x->key[i] is the first key > k; x->key[i-1] is the last key < k. */
		if (want_floor)
		{
			if (i > 0)
			{
				best = x->key[i - 1];
				*found = 1;
			}
		}
		else
		{
			if (i < x->n)
			{
				best = x->key[i];
				*found = 1;
			}
		}

		if (x->leaf)
		{
			break;
		}

		x = (BTNode*)x->kid[i];
	}

	return best;
}

static int64_t bt_bound_owned(void *o, int64_t k, int want_floor)
{
	int found;
	int64_t r = bt_bound(o, k, want_floor, &found);
	if (!found)
	{
		return 0;
	}

	if (bt_kman(o))
	{
		bzy_retain((void*)r);
	}

	return r;
}

int64_t bzy_btree_floor(void *o, int64_t k)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = bt_bound_owned(o, k, 1);
		bzy_shared_unlock(o);
		return r;
	}

	return bt_bound_owned(o, k, 1);
}

int64_t bzy_btree_ceiling(void *o, int64_t k)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = bt_bound_owned(o, k, 0);
		bzy_shared_unlock(o);
		return r;
	}

	return bt_bound_owned(o, k, 0);
}

/* In-order traversal collecting keys, values, or entries into a fresh array. */
typedef struct { int64_t *out; int64_t n; int want; void *tree; } BtCollect;   /* want: 0 keys, 1 values, 2 entries. */

static void bt_collect(void *o, BTNode *x, BtCollect *c)
{
	if (!x)
	{
		return;
	}

	for (int i = 0; i < x->n; i++)
	{
		if (!x->leaf)
		{
			bt_collect(o, (BTNode*)x->kid[i], c);
		}

		if (c->want == 0)
		{
			if (bt_kman(o))
			{
				bzy_retain((void*)x->key[i]);
			}

			c->out[c->n++] = x->key[i];
		}
		else if (c->want == 1)
		{
			if ((int)*HVMAN(o))
			{
				bzy_retain((void*)x->val[i]);
			}

			c->out[c->n++] = x->val[i];
		}
		else
		{
			void *e = bzy_entry_new(x->key[i], x->val[i], bt_kman(o), (int)*HVMAN(o));   /* Owned Entry. */
			c->out[c->n++] = (int64_t)(intptr_t)e;
		}
	}

	if (!x->leaf)
	{
		bt_collect(o, (BTNode*)x->kid[x->n], c);
	}
}

static void *bt_iter(void *o, int want)
{
	int managed_elem = want == 0 ? bt_kman(o) : (want == 1 ? (int)*HVMAN(o) : 1);
	void *arr = bzy_array_new(*HCOUNT(o), managed_elem ? 1 : 0);
	BtCollect c;
	c.out = (int64_t*)((char*)arr + 32);
	c.n = 0;
	c.want = want;
	c.tree = o;
	bt_collect(o, bt_root(o), &c);
	return arr;
}

void *bzy_btree_keys(void *o)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		void *a = bt_iter(o, 0);
		bzy_shared_unlock(o);
		return a;
	}

	return bt_iter(o, 0);
}

void *bzy_btree_values(void *o)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		void *a = bt_iter(o, 1);
		bzy_shared_unlock(o);
		return a;
	}

	return bt_iter(o, 1);
}

void *bzy_btree_entries(void *o)
{
	if (bt_shared(o))
	{
		bzy_shared_lock(o);
		void *a = bt_iter(o, 2);
		bzy_shared_unlock(o);
		return a;
	}

	return bt_iter(o, 2);
}
