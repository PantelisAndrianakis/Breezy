#ifndef BZY_COROUTINE_H
#define BZY_COROUTINE_H

typedef struct BzyCoroutine BzyCoroutine;
typedef void (*BzyCoroutineFn)(void *arg);

void          bzy_coroutine_main_init(void);                       /* Promote the current OS thread to a coroutine. */
BzyCoroutine *bzy_coroutine_thread_enter(void);                    /* Promote the CURRENT thread to its scheduler coroutine (idempotent, per thread). */
BzyCoroutine *bzy_coroutine_create(BzyCoroutineFn fn, void *arg);  /* A new suspended coroutine with its own stack. */
void          bzy_coroutine_switch(BzyCoroutine *to);              /* Save the current coroutine, resume `to`. */
BzyCoroutine *bzy_coroutine_self(void);                            /* The currently running coroutine. */
void          bzy_coroutine_delete(BzyCoroutine *c);

/* Caller-owned slot that rides with the coroutine through the recycling pool (it is
   not cleared on reuse). The scheduler stashes its per-breeze bookkeeping struct here
   so a recycled coroutine brings its Breeze back too - pooling both with one pop. */
void         *bzy_coroutine_attach(BzyCoroutine *c);
void          bzy_coroutine_set_attach(BzyCoroutine *c, void *p);

#endif
