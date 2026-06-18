# HTTP

`Http` is a **static namespace** for reading and writing HTTP/1.1 messages over a
[`Socket`](../io/native-io.md). It is a **codec, not a framework**: it parses a
request or response off a socket and builds one to write back, reusing the existing
`Network.listen`/`accept`/`connect` transport. You write the accept loop; `Http`
turns bytes into `HttpRequest`/`HttpResponse` values and back. For a one-shot GET
without running a server, [`Network.readUrl`](../io/native-io.md) is simpler.

← [Back to the guide](../guide.md)

---

## A server

One breeze per connection, a keep-alive loop:

```breezy
void main()
{
	Listener l = Network.listen(8080);
	while (true)
	{
		Socket c = l.accept();
		spawn serve(c);
	}
}

void serve(Socket c)
{
	HttpRequest req = Http.readRequest(c);     // null at a clean close.
	while (req != null)
	{
		if (req.method.equals("POST") && req.path.equals("/echo"))
		{
			Http.respond(c, 200, req.body);
		}
		else
		{
			Http.respond(c, 404, "not found");
		}
		req = Http.readRequest(c);              // The next request on the same socket.
	}
	c.close();
}
```

`Http.readRequest(socket)` parses one request and returns an `HttpRequest`, or
**`null`** when the peer closes the connection at a message boundary — so the
`while (req != null)` loop ends gracefully on a keep-alive connection. A malformed
message, or an EOF in the middle of one, **throws a catchable `HttpException`**.

`Http.respond(socket, status, body)` is the one-call reply: it writes the status
line (the reason is filled in from the code — `200 OK`, `404 Not Found`, …), a
`Content-Length`, a default `Content-Type: text/plain; charset=utf-8`, and the body.

---

## A client

```breezy
Socket s = Network.connect("example.com", 80);

HttpRequest q = Http.request("GET", "/health");
q.setHeader("Host", "example.com");
q.send(s);

HttpResponse r = Http.readResponse(s);
print(r.status);                 // e.g. 200.
print(r.header("Content-Type")); // The response header.
print(r.body);                   // The body.
s.close();
```

`Http.request(method, path)` builds a request; `setHeader`/`setBody` fill it in and
`send(socket)` writes it. `Http.readResponse(socket)` parses the reply into an
`HttpResponse` (or `null` at a clean close, `HttpException` on a malformed one).

---

## The messages

An `HttpRequest` and an `HttpResponse` share a header + body surface; each adds its
own start-line fields.

| Member | Type | On | Meaning |
| --- | --- | --- | --- |
| `req.method` | `string` | request | The method (`"GET"`, `"POST"`, …). |
| `req.path` | `string` | request | The request target (path + query). |
| `req.version` | `string` | request | The HTTP version token (`"HTTP/1.1"`). |
| `resp.status` | `int` | response | The status code (`200`, `404`, …). |
| `resp.reason` | `string` | response | The reason phrase (`"OK"`). |
| `m.header(name)` | `string` | both | The header value, or `""` when absent (**case-insensitive**). |
| `m.hasHeader(name)` | `bool` | both | Whether the header is present (case-insensitive). |
| `m.headerNames()` | `List<string>` | both | The header names, in received order. |
| `m.body` | `string` | both | The body bytes (length-counted; binary-safe). |
| `m.bodyBytes()` | `byte[]` | both | An owned `byte[]` copy of the body. |

Header lookup is **case-insensitive** (`header("content-type")` and
`header("Content-Type")` are the same), matching HTTP. The body is a
length-counted string, so it carries text or binary equally; pair it directly with
[`Json.parse(req.body)`](json.md) for a JSON API, or read `bodyBytes()` for binary.

Building a message with custom headers (e.g. a JSON content type) uses the same
`setHeader`/`setBody` on a value from `Http.response(status)` or
`Http.request(method, path)`:

```breezy
HttpResponse r = Http.response(200);
r.setHeader("Content-Type", "application/json");
r.setBody(Json.stringify(payload));
r.send(c);                            // Status line + headers + Content-Length + body.
```

`setHeader` adds a header, or replaces it on a case-insensitive name match. `send`
derives `Content-Length` from the body and writes the whole message in one socket
write.

---

## Framing and connections

- **Keep-alive by default.** A connection serves many sequential request/response
  exchanges; the server loops `readRequest` until it returns `null` (peer closed)
  or the request carried `Connection: close`.
- **Request bodies** are framed by `Content-Length`, or decoded from **chunked**
  transfer-encoding (`5\r\nHello\r\n0\r\n\r\n`). **Responses** are always written
  with `Content-Length`.

---

## Errors

A malformed start line or header, a bad chunk, or an EOF in the middle of a message
throws an [`HttpException`](../language/exceptions.md) you can catch:

```breezy
try
{
	HttpRequest req = Http.readRequest(c);
	// ... handle req ...
}
catch (HttpException e)
{
	print(e.message);   // e.g. "Malformed request line."
}
```

A **clean** EOF at a message boundary is **not** an error — `readRequest` /
`readResponse` return `null` so a keep-alive loop ends quietly.

---

## v1 boundaries

- **HTTP/1.1 only** — no HTTP/2.
- **Plain `Socket`** — HTTPS is reachable later by composing with a
  [`TlsSocket`](native-io.md) transport; the codec itself does not do TLS.
- **Chunked decode on read; responses are `Content-Length`** (no chunked write).
- **Keep-alive but not pipelining** — read each response before sending the next
  request on a reused connection.
- **No routing, gzip, multipart, cookies, or redirect-following** — you write the
  accept loop and dispatch on `req.path`; richer policy is yours to add.
- **`Network.readUrl`** stays as the convenience one-shot GET client.

A parsed message is an ordinary managed value: reference-counted and reclaimed when
its last reference goes away.

---

← [Back to the guide](../guide.md) · [JSON](json.md) · [Native I/O](../io/native-io.md)
