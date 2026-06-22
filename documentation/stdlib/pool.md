# Pool

`Pool<T>` is a **bounded pool of reusable resources**: it holds a fixed number of
`T`s, hands one out on `acquire()`, and takes it back on `release()`. Its first use
is a **connection pool** — `Pool<PgConnection>` / `Pool<MyConnection>` — so a server
pays a database's TCP + authentication handshake a handful of times at start-up
instead of on every query. The same class pools any reusable resource (a `Socket`,
a buffer, anything).

It is an ordinary generic class compiled into every program (like `Option` and
`Result`), built on the [channel](../concurrency/channels.md) primitive — so it
costs nothing until you instantiate one.

← [Back to the guide](../guide.md)

---

## The shape

```breezy
// A factory that produces one resource. Bind it to a ()->T variable first
// (a lambda written inline at the `new` call cannot infer its type there).
()->PgConnection open = () => Postgres.connect("127.0.0.1", 5432, "app", "secret", "shop");

// Open 8 connections up front.
Pool<PgConnection> pool = new Pool<PgConnection>(8, open);

PgConnection db = pool.acquire();            // parks if all 8 are checked out
DbResult r = Postgres.query(db, "select 1");
pool.release(db);                            // back in the pool, no re-handshake
```

`new Pool<T>(int size, ()->T factory)` calls `factory` `size` times and fills the
pool. `acquire()` returns a free resource; if every resource is currently checked
out, the calling breeze **parks** until one is released (this is the pool's
backpressure — work waits rather than opening unbounded connections). `release(r)`
returns a resource to the pool.

> **Bind the factory to a variable.** A lambda written directly as the second
> argument (`new Pool<PgConnection>(8, () => ...)`) has no type to infer from a
> generic constructor parameter, so assign it to a `()->T` variable first, as
> above.

---

## Why it pays

Opening a database connection is expensive — a TCP round trip plus an
authentication handshake (SCRAM for PostgreSQL, the native or caching-SHA-2
exchange for MySQL). Measured against a live PostgreSQL, a fresh `connect` per
point-select costs **~4.0 ms/query**; reusing a pooled connection costs
**~139 µs/query** — about **28× faster**, the difference being exactly the
handshake the pool no longer repeats. A pooled query is no slower than a query on a
hand-held connection, so the pool only removes cost, it adds none.

---

## Works with any resource

`T` is any type, including the built-in handle types. A pool of raw sockets:

```breezy
()->Socket dial = () => Network.connect("10.0.0.5", 6379);
Pool<Socket> conns = new Pool<Socket>(16, dial);

Socket s = conns.acquire();
// ... use s ...
conns.release(s);
```

---

## v1 boundaries

The pool is deliberately small; each item below is a planned follow-up, not a
hidden limitation.

- **Fixed size.** The pool opens `size` resources at construction and never grows
  or shrinks. Lazy growth to a maximum is a follow-up.
- **No validation on borrow.** A resource handed back is assumed still live.
  Pooling connections that a server may have closed while idle will want a
  validate-on-acquire hook; until then, a long-idle pool can hand out a dead
  connection.
- **No acquire timeout.** `acquire()` parks until a resource frees. A bounded-wait
  variant is a follow-up.
- **No bulk close.** To shut a pool down, `acquire()` every resource and close each
  yourself; otherwise process exit reclaims them.

`Pool` is a built-in name (like `Option`, `DateTime`, and the database `Row`); a
class of your own may not also be called `Pool`.

---

← [Back to the guide](../guide.md)
