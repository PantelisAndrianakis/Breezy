# Native I/O - Synchronous to Write, Asynchronous Underneath

Every I/O call in Breezy **looks blocking and behaves non-blocking**. You write straight-line, sequential code; underneath, a single facade rides three backends so your scheduler thread never stalls. This is what lets one core serve tens of thousands of connections.

← [Back to the guide](../guide.md)

---

## One facade, three backends

- **Network** rides `epoll` (Linux), IOCP (Windows), or `kqueue` (BSD). A breeze waiting on a socket parks and frees the core for others.
- **Files** cannot be polled reliably, so file reads and writes are dispatched to a small **offload thread pool**: the breeze yields, a worker does the blocking syscall, and the breeze resumes. (On modern Linux, an `io_uring` backend slots in under the same API for true async file I/O.)
- **Blocking C calls** (a synchronous `mysql_query`, for example) use that same offload pool, so one slow database round-trip cannot freeze every other client on the core.

In every case the *code* looks blocking; the *runtime* parks the breeze instead of the thread.

---

## A TCP echo server

Written as if each call blocked - but every `accept`, `read`, and `write` parks the breeze on the completion port and frees the core:

```breezy
void main()
{
	Listener l = Network.listen(8080);
	while (true)
	{
		Socket c = l.accept();           // Parks until a client connects.
		spawn handle(c);          // Serve each client on its own breeze.
	}
}

void handle(Socket c)
{
	string line = c.readText(1024);      // Parks until bytes arrive.
	c.writeText(line);            // Echo it back (a full write).
	c.close();
}
```

`Network.listen(port)` returns a `Listener`; `accept()` yields a `Socket` per client. Pair this with one [breeze](../concurrency/breezes.md) per connection and the [zone model](../concurrency/zone-model.md) for a service that scales across cores.

---

## Fetch a URL in one call

`Network.readUrl(url)` is the high-level HTTP client. It fetches an `http://` **or** `https://` URL and returns the response **body** as a string. TLS, redirects, and chunked transfer-encoding are handled natively (WinHTTP on Windows, libcurl on Linux), and the request runs on the offload pool - so the breeze parks while a worker thread does the work and your scheduler core keeps flowing. A transport or URL error (bad URL, DNS failure, TLS error) throws a catchable [`IOException`](../stdlib/file.md).

```breezy
string page = Network.readUrl("https://example.com");   // Parks the breeze; a worker does TLS + GET.
print(page.contains("Example Domain"));           // true.
```

`readUrl` is GET-only and returns just the body (no status or headers surface yet). For full control - other methods, headers, status - drop down to a raw `Socket`.

---

## Rules & gotchas

- **Every I/O call looks blocking but parks the breeze**, not the OS thread.
- **Network uses the OS completion/readiness mechanism**; files and blocking C calls use the offload pool.
- **`Network.listen` / `accept` / `readText` / `writeText` / `close`** are the core TCP surface.
- **`Network.readUrl` is one-call HTTP/HTTPS GET**, returns the body, and throws `IOException` on failure.
- **For more than a GET body**, use a raw `Socket`.

---

← [The zone model](../concurrency/zone-model.md) · [Back to the guide](../guide.md) · Next: [File writing & logging](file-writing.md)
