# PostgreSQL

`Postgres` is a **static namespace** for talking to a PostgreSQL server. It is a
native **wire-protocol driver** — it speaks the PostgreSQL v3 frontend/backend
protocol directly over the existing [`Socket`](../io/native-io.md) transport, with
**no `libpq` dependency**. Because it reuses the parked socket I/O, a query
**suspends its breeze** while it waits on the server instead of blocking a worker
thread, so one thread can carry many concurrent connections.

← [Back to the guide](../guide.md)

---

## Connect, query, read

```breezy
PgConnection db = Postgres.connect("127.0.0.1", 5432, "app", "secret", "shop");

DbResult r = Postgres.query(db, "select id, name, price from items where price > $1", ["100"]);
for (int i = 0; i < r.rowCount(); i = i + 1)
{
	Row row = r.row(i);
	print(row.getLong("id") + " " + row.getString("name") + " " + row.getDouble("price"));
}

db.close();
```

`Postgres.connect(host, port, user, password, database)` opens a connection,
authenticates, and returns a `PgConnection` — or throws a catchable
[`DbException`](../language/exceptions.md) if the host is unreachable or the
credentials are rejected. `db.close()` ends the session.

---

## Encrypted connections (TLS)

`Postgres.connectTls(host, port, user, password, database)` is the same as
`connect`, but every byte after the handshake is encrypted. It negotiates the
upgrade the way PostgreSQL expects — an `SSLRequest` over the plain socket, then a
TLS handshake over that same socket — and then runs startup, authentication, and
all queries over the encrypted channel. The returned `PgConnection` is used exactly
like a plaintext one.

```breezy
PgConnection db = Postgres.connectTls("db.internal", 5432, "app", "secret", "shop");
DbResult r = Postgres.query(db, "select 1");   // encrypted
db.close();
```

`connectTls` **verifies the server certificate and hostname** against the system
trust store, and throws a `DbException` if verification fails — so a man-in-the-middle
is rejected by default. A server that does not offer TLS is an error, **never** a
silent downgrade to plaintext.

For a server with a self-signed or otherwise unverifiable certificate (a development
box), `Postgres.connectTlsInsecure(...)` takes the same arguments but **skips
verification**. Do not use it in production — it accepts any certificate.

> TLS is provided by the system OpenSSL, loaded on demand (the same library the
> driver already loads for SCRAM authentication). A plaintext `connect` never starts
> a TLS handshake.

---

## Queries

`Postgres.query(connection, sql)` runs a statement and returns a `DbResult`:

```breezy
DbResult r = Postgres.query(db, "select count(*) as n from items");
print(r.row(0).getLong("n"));
```

`Postgres.query(connection, sql, params)` binds parameters **on the wire** — the
values are never spliced into the SQL text, so the query is **injection-safe** by
construction. Placeholders are `$1`, `$2`, … and `params` is a `string[]`; a `null`
slot binds SQL `NULL`:

```breezy
string[] p = new string[2];
p[0] = "" + userId;        // Values are passed as text; the server casts them.
p[1] = "active";
DbResult r = Postgres.query(db, "select * from sessions where user_id = $1 and state = $2", p);
```

> **Always pass user input as a parameter, never by string-concatenating it into the
> SQL.** `query(db, "... where name = $1", [name])` is safe; building the SQL with
> `"... where name = '" + name + "'"` is not.

A statement that returns no rows (`insert`, `update`, `create`, …) still returns a
`DbResult`; its `rowCount()` is the number of rows affected.

---

## Results

A `DbResult` is a set of `Row`s plus the column names.

| Member | Type | On | Meaning |
| --- | --- | --- | --- |
| `r.rowCount()` | `int` | result | Rows returned, or rows affected for a non-`select`. |
| `r.columnCount()` | `int` | result | Number of columns. |
| `r.row(i)` | `Row` | result | The i-th row (throws `DbException` if `i` is out of range). |
| `r.columnName(i)` | `string` | result | The i-th column's name. |
| `row.getString(col)` | `string` | row | The value as text (`""` for SQL `NULL`). |
| `row.getInt(col)` | `int` | row | The value parsed as a 32-bit int. |
| `row.getLong(col)` | `long` | row | The value parsed as a 64-bit int. |
| `row.getDouble(col)` | `double` | row | The value parsed as a double. |
| `row.getBool(col)` | `bool` | row | The value parsed as a bool (`t`/`true`/`1`). |
| `row.isNull(col)` | `bool` | row | Whether the value is SQL `NULL`. |
| `row.columnCount()` | `int` | row | Number of columns in the row. |

Every getter takes **either a column index (`int`) or a column name (`string`)** —
`row.getLong(0)` and `row.getLong("id")` are the same lookup. Values are stored as
text and parsed on demand, so a SQL `NULL` reads back as the type's zero (`""`, `0`,
`0.0`, `false`); use `isNull` to tell a real `NULL` from an empty value.

---

## Authentication

`Postgres.connect` negotiates whatever the server asks for:

- **SCRAM-SHA-256** — the modern default (`scram-sha-256` in `pg_hba.conf`).
- **MD5** — the older `md5` method.
- **Cleartext** — only safe over TLS; supported for completeness.

The crypto comes from the system OpenSSL (`libcrypto`), loaded on demand only when a
connection actually authenticates — a trust-auth connection pulls no crypto at all.

---

## Errors

A connection failure, a rejected password, or a SQL error from the server throws a
catchable [`DbException`](../language/exceptions.md); the message carries the
server's SQLSTATE and text:

```breezy
try
{
	DbResult r = Postgres.query(db, "select * from no_such_table");
}
catch (DbException e)
{
	print(e.message);   // e.g. "42P01: relation \"no_such_table\" does not exist"
}
```

The connection stays usable after a caught query error — the driver drains the
server's response so the next query starts clean.

---

## v1 boundaries

- **Plain `Socket`** — TLS (`Postgres.connectTls`, the `SSLRequest` upgrade) is a
  planned follow-up; v1 connects in the clear. (The same staging the HTTP codec
  used for HTTPS.)
- **Text result values** — every column reads back as text and the getters parse it;
  there is no binary result format yet.
- **Parameters are `string[]`** — pass numbers as their text; a typed-parameter API
  is a later addition.
- **One connection per `PgConnection`** — no built-in pool yet (build one over the
  [channel](../language/concurrency.md) primitive if you need it).
- **No cursors, `COPY`, `LISTEN`/`NOTIFY`, or a transaction object** — issue
  `begin`/`commit` as ordinary queries.
- **MySQL/MariaDB** is a **separate driver** (same `DbResult`/`Row` surface), not
  part of `Postgres`.

A `PgConnection`, `DbResult`, and `Row` are ordinary managed values: reference-counted
and reclaimed when their last reference goes away.

---

← [Back to the guide](../guide.md) · [JSON](json.md) · [Native I/O](../io/native-io.md)
