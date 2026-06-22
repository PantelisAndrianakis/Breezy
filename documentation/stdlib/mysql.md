# MySQL / MariaDB

`Mysql` is a **static namespace** for talking to a MySQL or MariaDB server — **one
driver serves both**, since MariaDB speaks the MySQL wire protocol. Like the
[`Postgres`](postgres.md) driver it is a native **wire-protocol driver**: it speaks
the MySQL client/server protocol directly over the existing
[`Socket`](../io/native-io.md), with **no `libmysqlclient` dependency**, and a query
**parks its breeze** while it waits on the server instead of blocking a worker
thread.

It produces the **same `DbResult` / `Row` values** as the PostgreSQL driver — see
[the result surface in the Postgres page](postgres.md#results) for the full accessor
table. Only the connection type and the namespace differ.

← [Back to the guide](../guide.md)

---

## Connect, query, read

```breezy
MyConnection db = Mysql.connect("127.0.0.1", 3306, "app", "secret", "shop");

DbResult r = Mysql.query(db, "select id, name from items where price > ?", ["100"]);
for (int i = 0; i < r.rowCount(); i = i + 1)
{
	Row row = r.row(i);
	print(row.getLong("id") + " " + row.getString("name"));
}

db.close();
```

`Mysql.connect(host, port, user, password, database)` opens a connection,
authenticates, and returns a `MyConnection` — or throws a catchable
[`DbException`](../language/exceptions.md) on a connection or auth failure.
`db.close()` ends the session.

---

## Encrypted connections (TLS)

`Mysql.connectTls(host, port, user, password, database)` is the same as `connect`,
but encrypted. It negotiates the upgrade the way MySQL expects — read the server
greeting, send an `SSLRequest` (the `CLIENT_SSL` capability), switch to TLS, then
complete the handshake and run all queries over the encrypted channel. The returned
`MyConnection` is used exactly like a plaintext one.

```breezy
MyConnection db = Mysql.connectTls("db.internal", 3306, "app", "secret", "shop");
DbResult r = Mysql.query(db, "select 1");   // encrypted
db.close();
```

`connectTls` **verifies the server certificate and hostname** and throws a
`DbException` if verification fails — a man-in-the-middle is rejected by default. A
server that does not offer TLS is an error, never a silent downgrade.
`Mysql.connectTlsInsecure(...)` takes the same arguments but **skips verification**,
for a development server with a self-signed certificate; do not use it in production.

TLS also enables **`caching_sha2_password` full authentication**: when the server has
no cached entry for the account, that scheme must send the password over a secure
channel — over `connectTls` this now works (over a plaintext `connect` it raises a
`DbException` pointing here). `mysql_native_password` works over either.

> TLS is provided by the system OpenSSL, loaded on demand. A plaintext `connect`
> never starts a TLS handshake.

---

## Queries

`Mysql.query(connection, sql)` runs a statement and returns a `DbResult`. A
statement with no result set (`insert`, `update`, `create`, …) returns a `DbResult`
whose `rowCount()` is the number of affected rows.

`Mysql.query(connection, sql, params)` binds parameters **on the wire** via a
prepared statement — the values are never spliced into the SQL text, so the query is
**injection-safe**. Placeholders are `?` and `params` is a `string[]`; a `null` slot
binds SQL `NULL`:

```breezy
string[] p = new string[2];
p[0] = "" + userId;        // Values are passed as text; the server casts them.
p[1] = "active";
DbResult r = Mysql.query(db, "select * from sessions where user_id = ? and state = ?", p);
```

> **Always pass user input as a parameter, never by string-concatenating it into the
> SQL.** `query(db, "... where name = ?", [name])` is safe; building the SQL with
> `"... where name = '" + name + "'"` is not.

The result of a parameterized query comes back over MySQL's **binary protocol**; the
driver decodes each column (integers, floats, dates/times, and the text/blob
families) into the shared text-valued `Row` model, so the getters work the same as
for any other query.

---

## Authentication

`Mysql.connect` negotiates the server's auth plugin:

- **`caching_sha2_password`** — the MySQL 8 default. The **fast path** (the server has
  the password cached) is supported. The **full-auth path** (first login of a fresh
  account) requires TLS or the server's RSA public key, which is the deferred
  `connectTls` follow-up — until then, a fresh `caching_sha2` account raises a clear
  `DbException` pointing at TLS.
- **`mysql_native_password`** — the MariaDB default and the classic MySQL method.
  Fully supported.

The crypto comes from the system OpenSSL (`libcrypto`), loaded on demand only when a
connection authenticates.

---

## Errors

A connection failure, a rejected password, or a SQL error from the server throws a
catchable [`DbException`](../language/exceptions.md) carrying the server's SQLSTATE
and message:

```breezy
try
{
	DbResult r = Mysql.query(db, "select * from no_such_table");
}
catch (DbException e)
{
	print(e.message);   // e.g. "42S02: Table 'shop.no_such_table' doesn't exist"
}
```

The connection stays usable after a caught query error — the driver drains the
server's response so the next query starts clean.

---

## v1 boundaries

- **Plain `Socket`** — TLS (`connectTls`) is a planned follow-up; v1 connects in the
  clear. This also gates `caching_sha2_password` full auth (above).
- **Text result values** — every column reads back as text and the getters parse it.
- **Parameters are `string[]`** — bound as text-format (`VAR_STRING`) prepared
  parameters; a typed-parameter API is a later addition.
- **One connection per `MyConnection`** — no built-in pool yet.
- **No multi-statement scripts, stored-procedure result sets, `LOAD DATA LOCAL
  INFILE`, or a transaction object** — issue `begin`/`commit` as ordinary queries.
- **PostgreSQL** is a **separate driver** ([`Postgres`](postgres.md)) sharing the same
  `DbResult`/`Row` surface.

A `MyConnection`, `DbResult`, and `Row` are ordinary managed values: reference-counted
and reclaimed when their last reference goes away.

---

← [Back to the guide](../guide.md) · [PostgreSQL](postgres.md) · [Native I/O](../io/native-io.md)
