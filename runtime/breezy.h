#ifndef BREEZY_H
#define BREEZY_H
#include <stdint.h>

/* Object header: [vtable @0][refcount @8][gcinfo @16][fields @24..].
   The type descriptor sits at vtable - 8 and holds:
   [num_obj_fields @0][offset0 @8][offset1 @16]....
   gcinfo packs the cycle-collector state: bits 0-1 color, bit 2 buffered,
   bits 8+ the cyclic refcount (crc) scratch used during trial deletion. */

/* Set in gcinfo (bit 3) at allocation for classes whose instances may cross a
   core boundary; bzy_retain/bzy_release then use atomic refcount ops. Bits 0-2
   hold color/buffered and bits 8+ hold the crc, so bit 3 is free and survives
   every existing gcinfo update. Codegen emits this literal at `new` (cg_new). */
#define BZY_GCINFO_SHARED (1ll << 3)

void   *bzy_alloc(int64_t size);   /* Allocate, zero, set refcount to 1, bump live count; caller sets the vtable. */
void    bzy_retain(void *obj);     /* Increment the refcount (NULL-safe). */
void    bzy_release(void *obj);    /* Decrement the refcount; free acyclic garbage, buffer cycle candidates (NULL-safe). */
void    bzy_share_crosscore(void *o); /* Deep-share an object graph before a cross-core handoff (channel send / spawn arg / static store). */
void    bzy_shared_lock(void *o);     /* Stripe lock for shared-container ops (address-hashed). */
void    bzy_shared_unlock(void *o);
int64_t bzy_array_get_shared(void *slot);            /* Locked slot load+retain (owned); managed elements, bounds pre-checked. */
void    bzy_array_set_shared(void *slot, int64_t v); /* Locked slot swap; deep-shares and consumes the +1 on v, releases old outside the stripe. */
int64_t bzy_live_count(void);      /* Number of objects currently alive (for tests and leak checks). */

void   *bzy_class_name(void *obj);  /* Owned (+1) string: the object's dynamic class name. */
void    bzy_collect_cycles(void);  /* Run trial deletion over the buffered cycle-root candidates. */
int64_t bzy_roots_buffered(void);  /* Number of candidate roots pending collection (for tests). */

void    bzy_print_i64(int64_t v);  /* Print a signed integer as %lld followed by a newline. */
void    bzy_print_u64(uint64_t v); /* Print an unsigned integer as %llu followed by a newline. */
void    bzy_print_bool(int64_t v); /* Print "true" or "false" followed by a newline. */
void    bzy_print_f64(double v);   /* Print a double with %.17g followed by a newline. */

void   *bzy_str_new(const char *bytes, int64_t len); /* Owned (+1) immutable string. */
void   *bzy_str_from_cstring(const char *p);         /* Copy a NUL-terminated char* into an owned string; NULL -> null. */
void   *bzy_str_from_cbytes(const char *p, int64_t len); /* Copy len bytes into an owned string; NULL -> null. */
void   *bzy_str_to_bytes(void *s);                   /* Owned (+1) byte[] copy of the string's UTF-8 bytes. */
void   *bzy_str_from_bytes(void *arr);               /* Owned (+1) string from a byte[]/ubyte[]; NULL -> null. */
int64_t bzy_str_len(void *s);                        /* Byte length (excludes the NUL). */
const char *bzy_str_data(void *s);                   /* Pointer to the inline NUL-terminated bytes. */
void   *bzy_str_concat(void *a, void *b);            /* Owned (+1) a followed by b. */
void   *bzy_str_concat_n(void **parts, int64_t n);   /* Owned (+1): parts[0..n) joined in one allocation. */
void   *bzy_str_from_i64(int64_t v);                 /* Owned (+1) decimal text of a signed integer. */
void   *bzy_str_from_u64(uint64_t v);                /* Owned (+1) decimal text of an unsigned integer. */
void   *bzy_str_from_bool(int64_t v);                /* Owned (+1) "true"/"false". */
void   *bzy_str_from_f64(double v);                  /* Owned (+1) text of a double (matches print). */
int64_t bzy_str_eq(void *a, void *b);                /* 1 if byte-equal (NULL-safe, identity fast path). */
int64_t bzy_str_hashcode(void *s);                   /* 32-bit FNV-1a content hash (NULL-safe). */
void   *bzy_str_hashslot(void *s);                   /* Addr of the 8-byte cached map-hash slot (after the NUL). */
int64_t bzy_ptr_hash(void *p);                       /* Identity hash of a pointer (NULL-safe). */
int64_t bzy_str_contains(void *s, void *needle);     /* 1 if needle occurs in s (empty needle -> 1). */
int64_t bzy_str_starts_with(void *s, void *pre);     /* 1 if s begins with pre. */
int64_t bzy_str_ends_with(void *s, void *suf);       /* 1 if s ends with suf. */
int64_t bzy_str_index_of(void *s, void *needle);     /* First byte index of needle, or -1 (empty -> 0). */
void   *bzy_str_substring(void *s, int64_t start, int64_t end); /* Clamped to [0,len]; owned. */
void   *bzy_str_replace(void *s, void *from, void *to);         /* All literal occurrences; owned. */
void   *bzy_str_trim(void *s);                                  /* Strip ASCII whitespace ends; owned. */
void   *bzy_str_to_upper(void *s);                              /* ASCII upper; owned. */
void   *bzy_str_to_lower(void *s);                              /* ASCII lower; owned. */
int64_t bzy_str_equals_ignore_case(void *a, void *b);  /* ASCII case-insensitive equality. */
void *bzy_net_read_url(void *url);                     /* HTTP/HTTPS GET -> body string (fallible). */
void  bzy_set_args(int argc, char **argv);             /* Capture argv at startup (entry.c). */
void *bzy_sys_args(void);                              /* System.args() -> string[] of user args. */
void *bzy_sys_getenv(void *name);                     /* System.getenv(name) -> owned env value, or null when unset. */
void    bzy_await_shutdown(void);                     /* Park the breeze until SIGINT/SIGTERM (Ctrl+C on Windows). */
void    bzy_sys_sleep(int64_t ms);                    /* System.sleep(ms): park ~ms milliseconds (offloaded). */
void    bzy_sys_raw_mode(int64_t on);                 /* System.rawMode(on): raw terminal (char-at-a-time, no echo), auto-restored. */
int64_t bzy_sys_poll_key(void);                       /* System.pollKey(): next pending input byte (0..255), or -1. */
void    bzy_sys_mouse_mode(int64_t on);               /* System.mouseMode(on): terminal mouse reporting, auto-restored. */
int64_t bzy_sys_poll_mouse(void);                     /* System.pollMouse(): next packed mouse event, or -1. */
int64_t bzy_sys_cpu_count(void);                      /* System.cpuCount(): online logical cores. */
int64_t bzy_sys_affinity(int64_t mask);               /* System.affinity(mask): pin current OS thread; 1 ok, 0 fail. */
int64_t bzy_str_is_empty(void *s);                     /* 1 if length 0. */
int64_t bzy_str_is_numeric(void *s);                   /* 1 if non-empty and all digits 0-9. */
int64_t bzy_str_is_alphanumeric(void *s);              /* 1 if non-empty and all letters/digits. */
int64_t bzy_str_char_at(void *s, int64_t i);           /* Byte at i as int, or -1 out of range. */
int64_t bzy_str_last_index_of(void *s, void *needle);  /* Last byte index of needle, or -1 (empty -> len). */
void   *bzy_str_repeat(void *s, int64_t n);            /* s repeated n times (n<=0 -> ""); owned. */
void   *bzy_str_split(void *s, void *sep);             /* Split on sep -> owned string[] (empty sep -> [s]). */

/* String -> number parsing. On malformed input (empty, trailing garbage, or out of
   range) each sets a thread-local error; the codegen-emitted bzy_number_check then
   throws NumberFormatException. Leading whitespace is tolerated. toBool accepts
   "true"/"false" case-insensitively. */
int64_t bzy_str_to_int(void *s);
int64_t bzy_str_to_long(void *s);
int64_t bzy_str_to_byte(void *s);
int64_t bzy_str_to_short(void *s);
float   bzy_str_to_float(void *s);
double  bzy_str_to_double(void *s);
int64_t bzy_str_to_bool(void *s);
void    bzy_number_check(int64_t pc, int64_t frame);   /* Throw NumberFormatException if a parse failed. */

void    bzy_print_str(void *s);                      /* Write the bytes + '\n'. */

void   *bzy_sb_new(void);                            /* Owned (+1) empty StringBuilder. */
void    bzy_sb_append(void *sb, void *s);            /* Append a string's bytes. */
void    bzy_sb_append_cstr(void *sb, const char *bytes, int64_t len);
void   *bzy_sb_to_string(void *sb);                  /* Owned (+1) immutable snapshot. */

void   *bzy_array_new(int64_t n, int64_t elem_is_managed); /* Owned (+1) zeroed array. */
void   *bzy_array_new_sized(int64_t n, int64_t elem_size, int64_t elem_is_managed); /* Owned (+1) zeroed array, explicit element width. */
int64_t bzy_array_len(void *a);
void    bzy_oob(int64_t index, int64_t length, int64_t pc, int64_t frame); /* Throw IndexOutOfBounds (no return). */
void    bzy_oob_abort(int64_t index, int64_t length);                      /* Print + abort (no return). */

void   *bzy_map_new(int64_t key_kind, int64_t val_is_managed); /* Owned (+1). */
void    bzy_map_put(void *m, int64_t key, int64_t val);
int64_t bzy_map_get(void *m, int64_t key);   /* 0/NULL if absent; retains a managed value. */
int64_t bzy_map_put_if_absent(void *m, int64_t key, int64_t val); /* Atomic; returns the value now at key (retained when managed). */
int64_t bzy_map_get_or_default(void *m, int64_t key, int64_t dflt); /* Atomic; value or dflt (retained when managed, both branches). */
int64_t bzy_map_has(void *m, int64_t key);   /* 1 / 0. */
void    bzy_map_remove(void *m, int64_t key);
int64_t bzy_map_len(void *m);
int64_t bzy_map_iter(void *m, int64_t from);   /* Next full slot index >= from, or -1. */
void   *bzy_map_iter_snapshot(void *m);        /* Owned handle for foreach: m itself (retained) when confined, a frozen clone when shared. */
int64_t bzy_map_key_at(void *m, int64_t slot); /* Key at slot (borrowed; no retain). */
int64_t bzy_map_val_at(void *m, int64_t slot); /* Value at slot (borrowed; no retain). */
int64_t bzy_map_contains_value(void *m, int64_t needle, int64_t val_kind); /* 1 if any value equals needle. */
void   *bzy_map_keys(void *m, int64_t elem_size);     /* Owned K[] snapshot packed at elem_size bytes per slot. */
void   *bzy_map_values(void *m, int64_t elem_size);   /* Owned V[] snapshot packed at elem_size bytes per slot. */
void   *bzy_map_entries(void *m);  /* Owned Entry[] snapshot. */

void   *bzy_entry_new(int64_t key, int64_t val, int64_t key_managed, int64_t val_managed); /* Owned (+1). */
int64_t bzy_entry_key(void *e);    /* Key (managed -> +1 owned). */
int64_t bzy_entry_val(void *e);    /* Value (managed -> +1 owned). */

/* Ordering comparators for the ordered containers (own TU runtime/order.c). */
typedef int (*bzy_cmp_fn)(int64_t, int64_t);
bzy_cmp_fn bzy_order_cmp_for(int64_t elem_kind);   /* Primitive comparator by elem_kind; NULL for objects (4). */
int64_t    bzy_obj_compare(void *a, void *b, int64_t slot); /* Order object keys via Comparable.compareTo at a vtable slot. */

/* PriorityQueue<T>: binary min-heap (own TU runtime/pqueue.c). poll/peek return
   an owned managed value (like bzy_map_get). obj_slot = Comparable.compareTo
   vtable slot for object keys, -1 for primitives. */
void   *bzy_pq_new(int64_t elem_kind, int64_t obj_slot);
void    bzy_pq_add(void *o, int64_t v);
int64_t bzy_pq_poll(void *o);
int64_t bzy_pq_peek(void *o);
int64_t bzy_pq_size(void *o);

void   *bzy_vec_new(int64_t elem_kind);          /* Owned (+1). 0 int,1 float,2 double,3 string,4 object. */
int64_t bzy_vec_len(void *v);
void    bzy_vec_push_back(void *v, int64_t val); /* Retains a managed element. */
void    bzy_vec_push_front(void *v, int64_t val);
int64_t bzy_vec_pop_back(void *v);               /* Transfers out (owned); aborts if empty. */
int64_t bzy_vec_pop_front(void *v);
int64_t bzy_vec_get(void *v, int64_t i);         /* Bounds-checked; retains a managed value (owned). */
void    bzy_vec_set(void *v, int64_t i, int64_t val);  /* Bounds-checked; retain new / release old. */
int64_t bzy_vec_peek_back(void *v);              /* Retains (owned); aborts if empty. */
int64_t bzy_vec_peek_front(void *v);
void    bzy_vec_remove_at(void *v, int64_t i);   /* Bounds-checked; releases the removed managed element. */
int64_t bzy_vec_index_of(void *v, int64_t needle); /* First index equal per elem_kind, or -1. */
int64_t bzy_vec_contains(void *v, int64_t needle); /* 1 / 0. */

int64_t bzy_clock_millis(void);   /* Wall-clock milliseconds since the Unix epoch. */
int     bzy_current_wid(void);    /* Index of the worker running this thread (0 if none). */
int     bzy_on_worker(void);      /* 1 in a scheduler-worker context, 0 on a foreign thread. */
void    bzy_callback_enter(void); /* FFI: entering a foreign callback (brackets an extern call that gets a fn ptr). */
void    bzy_callback_leave(void); /* FFI: leaving a foreign callback. */
int     bzy_in_callback(void);    /* FFI: 1 if a foreign callback is running on this thread (park/throw forbidden). */
void    bzy_cycle_slice(int wid); /* Drain this worker's cycle candidates in a bounded slice (safepoint). */
int64_t bzy_clock_nanos(void);    /* High-resolution monotonic counter, in nanoseconds. */
void   *bzy_clock_date(int64_t millis);                /* "yyyy-MM-dd HH:mm:ss", local time; owned. */
void   *bzy_clock_date_fmt(int64_t millis, void *fmt); /* Java-style pattern, local time; owned. */

int64_t bzy_rnd_bool(void);                            /* 0 or 1. */
int64_t bzy_rnd_int(void);                             /* Full 32-bit signed range. */
int64_t bzy_rnd_long(void);                            /* Full 64-bit range. */
float   bzy_rnd_float(void);                            /* [0, 1). */
double  bzy_rnd_double(void);                           /* [0, 1). */
double  bzy_rnd_gaussian(void);                         /* Mean 0, stddev 1. */
int64_t bzy_rnd_get_i(int64_t bound);                   /* [0, bound). */
int64_t bzy_rnd_get_ii(int64_t origin, int64_t bound);  /* [origin, bound] inclusive. */
int64_t bzy_rnd_get_l(int64_t bound);
int64_t bzy_rnd_get_ll(int64_t origin, int64_t bound);
float   bzy_rnd_get_f(float bound);                     /* [0, bound). */
float   bzy_rnd_get_ff(float origin, float bound);      /* [origin, bound). */
double  bzy_rnd_get_d(double bound);
double  bzy_rnd_get_dd(double origin, double bound);
void    bzy_rnd_bytes(void *arr);                       /* Fill each byte[] element with [0,255]. */

void    bzy_throw(void *exc, int64_t pc, int64_t frame); /* Unwind the rbp chain; never returns. */
void    bzy_io_check(int64_t pc, int64_t frame);         /* Throw IOException if the last File op failed. */

/* Crash-safe breezes (6a-4): on an uncaught exception inside a breeze, bzy_throw
   prints the trace then calls bzy_sched_breeze_uncaught, which ends the breeze and
   switches back to the scheduler (a fiber switch, NOT setjmp/longjmp — the dead
   breeze's NASM frames carry no SEH unwind info, so longjmp across them crashes).
   Outside a breeze (unit tests / pre-scheduler) bzy_throw aborts as before. */
void    bzy_sched_breeze_uncaught(void);   /* End the running breeze; resume the scheduler (no return). */
int64_t bzy_uncaught_count(void);          /* Count of uncaught exceptions the scheduler survived. */

int64_t bzy_file_exists(void *path);      /* 1 if the path exists. */
int64_t bzy_file_is_file(void *path);     /* 1 if it exists and is a regular file. */
int64_t bzy_file_is_folder(void *path);   /* 1 if it exists and is a directory. */
void    bzy_file_create_file(void *path);       /* Create an empty file. */
void    bzy_file_create_folder(void *path);     /* mkdir -p. */
void    bzy_file_delete(void *path);            /* Delete a file or empty folder. */
void    bzy_file_delete_recursive(void *path);  /* Delete a folder tree. */
void   *bzy_file_read_text(void *path);         /* Owned string of the whole file. */
void   *bzy_file_read_lines(void *path);        /* Owned string[]; split on \n, \r stripped. */
void    bzy_file_write_text(void *path, void *content);   /* Create/overwrite. */
void    bzy_file_append_text(void *path, void *content);  /* Create/append. */
void   *bzy_file_read_bytes(void *path);        /* Owned byte[] (one byte per element). */
void    bzy_file_write_bytes(void *path, void *data);     /* data : byte[]. */
void   *bzy_file_list(void *folder);            /* Owned string[] of full paths in folder. */
void   *bzy_file_search(void *folder, void *pattern);          /* Glob match, non-recursive. */
void   *bzy_file_search_recursive(void *folder, void *pattern);/* Glob match over the subtree. */
void    bzy_file_set_attribute(void *path, int64_t attr, int64_t on);  /* Windows FILE_ATTRIBUTE_* bits. */
int64_t bzy_file_has_attribute(void *path, int64_t attr);              /* 1 if the attribute bit is set. */

int64_t bzy_regex_matches(void *pat, void *text);            /* Full match -> 1/0. */
int64_t bzy_regex_test(void *pat, void *text);              /* Search -> 1/0. */
void   *bzy_regex_find(void *pat, void *text);              /* Leftmost match substring (owned; "" if none). */
void   *bzy_regex_replace(void *pat, void *text, void *repl); /* All matches replaced (owned). */

void    bzy_sched_init(void);              /* Promote the OS thread to the scheduler coroutine. */
void    bzy_sched_set_workers(int n);      /* Set the worker-thread count before run (n<=0 => core count). */
void    bzy_spawn(void (*entry)(void));    /* Enqueue a new breeze running entry (no args, 6a-1). */
void    bzy_spawn_args(void (*thunk)(void*), void *arg); /* Enqueue a breeze that runs thunk(arg). */
void   *bzy_spawn_args_begin(void (*thunk)(void*));      /* Inline-arg spawn: returns the pooled Breeze's argbuf to fill. */
void    bzy_spawn_args_commit(void *argbuf);             /* Enqueue the breeze whose argbuf is `argbuf`. */
void   *bzy_sched_current(void);           /* Opaque handle to the running breeze. */
void    bzy_sched_park(void);              /* Suspend the running breeze (off the ready queue). */
void    bzy_sched_park_unlock(void *srwlock); /* Park; the scheduler releases the SRWLOCK after the switch. */
void    bzy_sched_wake(void *breeze);      /* Re-enqueue a parked breeze. */

/* Offload pool (6b-1): run a blocking task on a real OS-thread pool while the
   calling breeze parks. fn(ctx) runs on a worker; ctx is the caller's stack node
   (stable while parked); the worker only reads the caller's buffers. */
void    bzy_offload_run(void (*fn)(void*), void *ctx);   /* Submit + park; returns when fn has run. */
int64_t bzy_offload_inflight(void);   /* Breezes currently parked on an offload task. */
void    bzy_offload_shutdown(void);   /* Drain/join the pool (no-op if never started). */
void    bzy_sched_wake_external(void *breeze);   /* Wake a breeze from a non-scheduler thread. */

/* IOCP core (6b-2). IocpOp is opaque here; its full definition (with the embedded
   OVERLAPPED) lives in runtime/iocp.c. Callers allocate it on their stack via the
   BZY_IOCP_OP_SIZE byte blob and treat it through these functions. */
typedef struct IocpOp IocpOp;
#define BZY_IOCP_OP_SIZE 64      /* sizeof(struct IocpOp); static_assert'd in iocp.c. */

void  bzy_iocp_ensure(void);             /* Lazily create the port + completion thread (idempotent). */
void  bzy_iocp_associate(void *handle);  /* Associate a SOCKET (as void*) with the completion port. */
void  bzy_iocp_op_reset(IocpOp *op);     /* Zero the OVERLAPPED, init the hand-off lock, set the breeze. */
void *bzy_iocp_op_overlapped(IocpOp *op);/* &op->ov, to pass to WSARecv/WSASend/etc. */
void  bzy_iocp_park(IocpOp *op);         /* Park until the completion thread wakes this op. */
unsigned long bzy_iocp_op_bytes(IocpOp *op);  /* Bytes transferred (valid after resume). */
int   bzy_iocp_op_err(IocpOp *op);            /* 0 on success else a Winsock error (valid after resume). */
int64_t bzy_iocp_inflight(void);         /* Breezes parked on a network op (Windows: iocp.c; Linux: reactor_epoll.c). */
void  bzy_iocp_shutdown(void);           /* Stop the completion thread + close the port (no-op if unused). */
/* Linux epoll reactor (the IOCP counterpart): readiness-park primitives for sockets
   live in pollstate.h (bzy_poll_wait / bzy_reactor_deregister); the lifecycle hooks
   stay here so non-socket runtime code can drive shutdown. */
void  bzy_reactor_ensure(void);          /* Lazily create the epoll instance + reactor threads. */
void  bzy_reactor_shutdown(void);        /* Stop the reactor threads (no-op if unused). */

/* TCP sockets (6b-2): managed leaf objects holding a SOCKET fd; the finalizer
   closesocket()s. accept/connect/read park the calling breeze on the IOCP. */
void   *bzy_listener_new(int64_t port);       /* Bind+listen on 0.0.0.0:port (0 = ephemeral); owned (+1). */
void   *bzy_listener_accept(void *l);         /* Parks; returns an owned Socket for the next connection. */
void   *bzy_listener_accept_timeout(void *l, int64_t ms);  /* Parks up to ms; NULL on timeout. */
void   *bzy_listener_try_accept(void *l);     /* NULL if no connection pending; never parks. */
int64_t bzy_listener_port(void *l);           /* The actual bound port (resolves 0 -> assigned). */
void    bzy_listener_close(void *l);
void   *bzy_socket_connect(void *host, int64_t port);  /* Parks; owned (+1) connected Socket. */
void   *bzy_raw_socket(int64_t protocol);   /* Network.rawSocket: owned Socket; throws IOException when denied. */
void   *bzy_tls_connect(void *host, int64_t port);                  /* TLS client, system-default CAs. Owned TlsSocket; throws on failure. */
void   *bzy_tls_connect_ca(void *host, int64_t port, void *caBundle); /* TLS client, explicit PEM CA bundle. */
void   *bzy_tls_listen(int64_t port, void *certPath, void *keyPath);  /* Owned TlsListener; throws on failure. */
void   *bzy_tls_accept(void *l);                  /* Owned TlsSocket; parks + handshakes; throws on failure. */
int64_t bzy_tls_listener_port(void *l);           /* The listener's bound port. */
void   *bzy_tls_read(void *s, int64_t maxbytes);  /* Owned byte[] (len 0 = EOF); throws on error. */
int64_t bzy_tls_write(void *s, void *data);       /* byte[]; encrypts+sends all; returns count; throws on error. */
void    bzy_tls_close(void *s);                   /* TLS shutdown + release transport. */
void    bzy_tls_close_listener(void *l);          /* Close the listener + free ctx. */
void   *bzy_surface_open(int64_t w, int64_t h, void *title);   /* Owned Surface; throws if SDL2/display absent. */
void    bzy_surface_present(void *s, void *pixels);            /* Blit a w*h ARGB int[]; throws on length/device error. */
int64_t bzy_surface_poll_event(void *s);          /* Next packed event, 0 when none. */
int64_t bzy_surface_is_open(void *s);             /* 0 once the window is closed. */
void    bzy_surface_close(void *s);               /* Stop + join the render thread; idempotent. */
void   *bzy_glsurface_open(int64_t w, int64_t h, void *title); /* Owned GlSurface; throws if SDL2/GL/display absent. */
int64_t bzy_glsurface_poll(void *s);              /* Next packed event, 0 when none. */
void    bzy_glsurface_swap(void *s);              /* SDL_GL_SwapWindow; aborts if used off the owning thread. */
int64_t bzy_glsurface_isopen(void *s);            /* 0 once closed. */
void    bzy_glsurface_close(void *s);             /* Delete context + window; idempotent. */
void    bzy_dyn_set_resolver(void *(*r)(const char *name)); /* Register the active dynamic-extern resolver. */
void   *bzy_dynsym(const char *name);             /* Resolve via the active resolver; io_fail + NULL if absent. */
int64_t bzy_ffi_bind(void *path);                 /* dlopen(path) + register a dlsym resolver; 1 ok / 0 fail. */
void   *bzy_socket_read(void *s, int64_t maxbytes);    /* Parks; owned byte[] (length 0 = peer closed). */
void   *bzy_socket_read_timeout(void *s, int64_t maxbytes, int64_t ms);  /* NULL on timeout. */
void   *bzy_socket_try_read(void *s, int64_t maxbytes); /* NULL if no data ready; len 0 = EOF. */
void   *bzy_socket_read_text(void *s, int64_t maxbytes);/* Parks; owned string of what was read. */
void   *bzy_socket_read_text_timeout(void *s, int64_t maxbytes, int64_t ms);  /* NULL on timeout. */
void   *bzy_socket_try_read_text(void *s, int64_t maxbytes);  /* NULL if no data ready. */
int64_t bzy_socket_write(void *s, void *data);         /* byte[]; writes all; returns count. */
int64_t bzy_socket_write_text(void *s, void *str);     /* string; writes all bytes; returns count. */
void    bzy_socket_close(void *s);

/* UDP sockets (6b-2): connectionless datagrams. receive() parks and returns a
   Datagram carrying the payload + the sender's address (so a server can reply). */
void   *bzy_udp_new(int64_t port);                           /* Bind 0.0.0.0:port (0 = ephemeral); owned. */
int64_t bzy_udp_port(void *u);                               /* Bound port. */
int64_t bzy_udp_send_to(void *u, void *host, int64_t port, void *data);   /* data: byte[]; returns count. */
int64_t bzy_udp_send_text_to(void *u, void *host, int64_t port, void *str);
void   *bzy_udp_receive(void *u);                            /* Parks; owned Datagram. */
void   *bzy_udp_receive_timeout(void *u, int64_t ms);        /* Parks up to ms; NULL on timeout. */
void   *bzy_udp_try_receive(void *u);                        /* NULL if no datagram ready; never parks. */
void    bzy_udp_close(void *u);

/* Datagram: a received payload + its sender. object_size = 48,
   0 vtable | 8 rc | 16 gcinfo | 24 data(byte[]) | 32 host(string) | 40 port(int64).
   num_obj_fields = 2 (offsets 24, 32) -- data + host are managed children. */
void   *bzy_dgram_data(void *d);   /* Owned byte[] (+1). */
void   *bzy_dgram_text(void *d);   /* Owned string (+1) of the payload bytes. */
void   *bzy_dgram_host(void *d);   /* Owned string (+1) sender IP. */
int64_t bzy_dgram_port(void *d);   /* Sender port. */

/* Random-access file channel (6b-5): a managed handle over an overlapped Win32
   file HANDLE on the IOCP port. readAt/writeAt are async positioned I/O (park on
   the completion port, no offload hand-off); sync() forces durability via the
   offload pool; size/truncate are inline. The finalizer CloseHandle()s. */
void  bzy_io_fail(const char *msg);   /* Set the thread-local io-error (defined in file.c). */
void *bzy_filechannel_open(void *path);                       /* Owned (+1); read+write, OPEN_ALWAYS. */
void *bzy_filechannel_read_at(void *ch, int64_t offset, int64_t maxbytes);  /* Owned byte[] (len 0 = EOF). */
int64_t bzy_filechannel_read_into(void *ch, void *buf, int64_t offset, int64_t maxLen); /* Fill buf[0..min(maxLen,len)); returns bytes read (0=EOF). */
int64_t bzy_filechannel_write_at(void *ch, int64_t offset, void *data);     /* byte[]; writes all; count. */
int64_t bzy_filechannel_size(void *ch);                       /* Current size in bytes (-1 on error). */
void  bzy_filechannel_truncate(void *ch, int64_t size);       /* Set file length (grow or shrink). */
void  bzy_filechannel_sync(void *ch);                         /* FlushFileBuffers (offloaded). */
void  bzy_filechannel_close(void *ch);
int64_t bzy_filechannel_lock(void *ch);      /* Exclusive advisory whole-file lock; parks; 1 ok, 0 fail. */
void    bzy_filechannel_unlock(void *ch);    /* Release the advisory lock (best-effort). */
void   *bzy_mmap_map(void *fc);              /* Owned MappedFile; throws if empty/unmappable. */
int64_t bzy_mmap_size(void *m);
int64_t bzy_mmap_get_byte(void *m, int64_t i);
int64_t bzy_mmap_get_int(void *m, int64_t i);
int64_t bzy_mmap_get_long(void *m, int64_t i);
void    bzy_mmap_put_byte(void *m, int64_t i, int64_t v);
void    bzy_mmap_put_int(void *m, int64_t i, int64_t v);
void    bzy_mmap_put_long(void *m, int64_t i, int64_t v);
void    bzy_mmap_copy_into(void *m, void *dst, int64_t srcOff, int64_t n);
void    bzy_mmap_flush(void *m);             /* msync/FlushViewOfFile+FlushFileBuffers; parks; throws on error. */
void    bzy_mmap_close(void *m);             /* Unmap + close mapping; idempotent. */

/* Buffered file writer (6b-3): a managed handle over an open FILE* + a userspace
   buffer. write/writeLine/writeBytes memcpy into the buffer (no syscall); the buffer
   flushes to disk when it fills, on flush(), or on close() -- each real flush runs on
   the offload pool so the breeze parks. open is inline; the finalizer flushes+closes. */
void *bzy_filewriter_open(void *path, int64_t append, int64_t buf_bytes);  /* Owned (+1); 0 buf_bytes = 64 KiB. */
void  bzy_filewriter_write(void *w, void *str);        /* Buffer the string's bytes. */
void  bzy_filewriter_write_line(void *w, void *str);   /* Buffer the string + '\n'. */
void  bzy_filewriter_write_bytes(void *w, void *data); /* Buffer a byte[]'s bytes. */
void  bzy_filewriter_flush(void *w);                   /* Flush buffered bytes to disk (offloaded). */
void  bzy_filewriter_close(void *w);                   /* Flush + close (offloaded). */

/* Channel-fed logger (6b-4): bundles a bounded channel<string>, a FileWriter, and a
   dedicated logger breeze that drains the channel and writes each line. log() moves a
   string into the channel (parks only if the logger is far behind); close() sends a
   sentinel, then parks until the breeze has drained+flushed+closed and signalled. */
void *bzy_logger_open(void *path);            /* Owned (+1); opens path in append mode, spawns the breeze. */
void  bzy_logger_log(void *logger, void *str);/* Move the string into the channel (ownership transfers). */
void  bzy_logger_close(void *logger);         /* Drain, flush, close the file, join the logger breeze. */

/* System.shell (VB.NET Shell-style): run "cmd /c <command>". wait==0 -> launch
   async, return the process id (0 on failure). wait!=0 -> block until exit and
   return the exit code; that blocking path offloads so the breeze parks. */
int64_t bzy_system_shell(void *command, int64_t wait);

void   *bzy_channel_new(int64_t cap, int64_t elem_managed); /* Owned (+1) bounded channel. */
void    bzy_channel_send(void *ch, int64_t v);   /* Parks if full; moves a managed value in. */
int64_t bzy_channel_recv(void *ch);              /* Parks if empty; returns an owned value. */

/* Timers (6a-4): a shared min-heap of Timer objects keyed by absolute deadline.
   Timer is a managed leaf object (same layout idiom as channel). */
void   *bzy_timer_schedule(void (*entry)(void), int64_t first_deadline, int64_t period); /* Owned (+1). */
void   *bzy_timer_after(void (*entry)(void), int64_t delay_ms);                          /* Owned (+1). */
void   *bzy_timer_every(void (*entry)(void), int64_t delay_ms, int64_t period_ms);       /* Owned (+1). */
void    bzy_timer_cancel(void *t);
int64_t bzy_timer_next_deadline(void);   /* Nearest non-cancelled deadline, or -1 if none pending. */
void   *bzy_timer_pop_due(int64_t now);  /* Removes & returns the earliest due entry (heap ref transferred), or NULL. */
void    bzy_timer_reinsert(void *t, int64_t now);  /* Periodic re-insert: advance deadline past now, re-push. */
int64_t bzy_timer_count(void);           /* Entries currently in the heap (test/diagnostic). */
void    bzy_timer_reset(void);           /* Drop every entry (test hygiene). */
void    bzy_sched_nudge(void);           /* Wake one idle worker to re-evaluate its timer wait (no-op pre-run). */
void    bzy_yield(void);                   /* Cooperatively yield to the scheduler (no-op outside a breeze). */
void    bzy_sched_run(void);               /* Run ready breezes until the queue drains. */

#endif
