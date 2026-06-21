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

> **Speaking HTTP?** Wrap the same `Socket` with the [`Http`](../stdlib/http.md) codec - `Http.readRequest(c)` / `Http.respond(c, status, body)` on the server, `Http.request(...).send(s)` / `Http.readResponse(s)` on the client - to parse and build HTTP/1.1 messages over this transport.

---

## IPv6 and dual-stack - automatic

Listeners are **dual-stack**: a single `Network.listen(port)` accepts both IPv6 and IPv4 clients (IPv4 arrives as a v4-mapped address). `Network.connect` accepts an IPv6 literal, an IPv4 literal, or a hostname interchangeably - the address family is resolved for you:

```breezy
Socket a = Network.connect("::1", port);          // IPv6 literal.
Socket b = Network.connect("127.0.0.1", port);    // IPv4 literal.
Socket c = Network.connect("example.com", port);  // Hostname (either family).
```

There is no IPv4-only or IPv6-only mode and no API change - existing code gains IPv6 for free, on both platforms. UDP (`Network.udp`) is dual-stack the same way.

> **Transport security (TLS):** the `Network.readUrl` HTTP client already does HTTPS natively. For a raw `Socket`, TLS is reachable by binding a native TLS library through the [FFI surface](../ffi/c-interop.md) (blocking mode); first-class asynchronous server-side TLS is a future addition.

---

## Fetch a URL in one call

`Network.readUrl(url)` is the high-level HTTP client. It fetches an `http://` **or** `https://` URL and returns the response **body** as a string. TLS, redirects, and chunked transfer-encoding are handled natively (WinHTTP on Windows, libcurl on Linux), and the request runs on the offload pool - so the breeze parks while a worker thread does the work and your scheduler core keeps flowing. A transport or URL error (bad URL, DNS failure, TLS error) throws a catchable [`IOException`](../stdlib/file.md).

```breezy
string page = Network.readUrl("https://example.com");   // Parks the breeze; a worker does TLS + GET.
print(page.contains("Example Domain"));           // true.
```

`readUrl` is GET-only and returns just the body (no status or headers surface yet). For full control - other methods, headers, status - drop down to a raw `Socket`.

---

## Raw sockets

For low-level packet work (custom protocols, probes), open a raw IP socket. This
is **privilege-gated**: it needs `CAP_NET_RAW`/root on Linux or Administrator on
Windows, and throws a catchable [`IOException`](../stdlib/file.md) when the OS
denies it.

```breezy
try
{
	Socket s = Network.rawSocket(255);   // protocol number (e.g. an IPPROTO value).
	s.write(frame);                      // Send a raw packet (byte[]).
	byte[] pkt = s.read(2048);           // Receive the next packet, parking the breeze.
	s.close();
}
catch (IOException e)
{
	// No privilege -- expected off a root/admin context.
}
```

`Network.rawSocket(protocol)` returns an ordinary `Socket` handle, so
`read`/`write`/`close` work as usual and the handle is reactor-integrated
(`read` parks the breeze like any socket).

**Caveats.** Raw sockets are OS-restricted. On **Windows** they are
Administrator-only and **cannot send TCP/UDP** (a Microsoft restriction since XP
SP2); they remain usable for other protocols and for receiving. On **Linux**
they need `CAP_NET_RAW` (typically root).

---

## TLS

Wrap a connection in TLS with `Network.tlsConnect` (client) or `Network.tlsListen`
(server). OpenSSL is loaded dynamically at first use; if it is absent the call
throws a catchable [`IOException`](../stdlib/file.md). Every TLS read/write parks
the breeze on the reactor, so TLS scales like any other socket.

```breezy
// Client: verify the server against the system CA store (or a CA bundle).
TlsSocket s = Network.tlsConnect("example.com", 443);
s.write(request);
byte[] body = s.read(65536);
s.close();

// Server: present a certificate + key (PEM).
TlsListener l = Network.tlsListen(8443, "server-cert.pem", "server-key.pem");
TlsSocket c = l.accept();             // Parks; runs the handshake.
```

- `Network.tlsConnect(host, port) -> TlsSocket` - verify the peer against the
  **system-default CA paths**, checking the hostname.
- `Network.tlsConnect(host, port, caBundlePath) -> TlsSocket` - verify against a
  PEM CA bundle. **Required on Windows**, where OpenSSL does not read the OS
  certificate store.
- `Network.tlsListen(port, certPath, keyPath) -> TlsListener` - server identity
  from a PEM cert chain + private key; `accept()` runs the server handshake and
  `port()` reports the bound port.
- `TlsSocket.read(max)` / `write(byte[])` / `close()` - same semantics as
  `Socket`, encrypted.

Verification is always on (no insecure mode); a handshake or certificate failure
throws `IOException`.

## DTLS (over UDP)

DTLS is TLS for **datagrams**. The surface mirrors TLS exactly; only the transport
differs (a connected UDP socket instead of a stream), so code abstracting over "a
secure connection" reads the same. OpenSSL is loaded dynamically at first use.

```breezy
// Client: connect + DTLS handshake over UDP.
DtlsSocket s = Network.dtlsConnect("example.com", 4433);
s.write(packet);                      // One encrypted record -> one datagram.
byte[] reply = s.read(1200);          // One decrypted application record.
s.close();

// Server (POSIX): present a certificate + key (PEM).
DtlsListener l = Network.dtlsListen(4433, "server-cert.pem", "server-key.pem");
DtlsSocket c = l.accept();            // Parks: discovers a peer, runs the handshake.
```

- `Network.dtlsConnect(host, port)` / `dtlsConnect(host, port, caBundlePath)` ->
  `DtlsSocket` - verify the peer like `tlsConnect`.
- `Network.dtlsConnectInsecure(host, port) -> DtlsSocket` - handshake **without**
  peer-cert verification. Loudly named so it is never reached by accident; for
  talking to a server with a self-signed or unknown cert.
- `Network.dtlsListen(port, certPath, keyPath) -> DtlsListener` - one connected
  socket per peer; `accept()` discovers the next peer and runs the server
  handshake, `port()` reports the bound port. **POSIX only** in this release
  (the Windows server is a follow-up); the DTLS *client* works on both platforms.
- `DtlsSocket.read(max)` / `write(byte[])` / `close()` - one record each; keep a
  `write` payload at or below the link MTU (~1200 bytes).

The runtime pins a conservative 1200-byte link MTU and drives DTLS handshake
retransmission itself (UDP does not guarantee delivery), so a lost handshake flight
is re-sent rather than treated as a dead connection.

---

## Rules & gotchas

- **Every I/O call looks blocking but parks the breeze**, not the OS thread.
- **Network uses the OS completion/readiness mechanism**; files and blocking C calls use the offload pool.
- **`Network.listen` / `accept` / `readText` / `writeText` / `close`** are the core TCP surface.
- **Listeners are dual-stack** (accept IPv6 and IPv4); `Network.connect` takes an IPv6 literal, IPv4 literal, or hostname - no API change.
- **`Network.readUrl` is one-call HTTP/HTTPS GET**, returns the body, and throws `IOException` on failure.
- **For more than a GET body**, use a raw `Socket`.
- **`Network.rawSocket(protocol)` is privilege-gated** - needs `CAP_NET_RAW`/root or Administrator, throws `IOException` when denied, and on Windows cannot send TCP/UDP.
- **`Network.tlsConnect` / `tlsListen` need OpenSSL at runtime** (loaded dynamically) and throw `IOException` when it is absent or when verification fails; on Windows, pass a CA-bundle path since OpenSSL does not read the Windows certificate store.
- **`Network.dtls*` is DTLS over UDP** - same OpenSSL dependency as TLS; the server (`dtlsListen`/`accept`) is POSIX-only in this release, the client works everywhere, and `dtlsConnectInsecure` skips peer verification on purpose.

---

← [The zone model](../concurrency/zone-model.md) · [Back to the guide](../guide.md) · Next: [File writing & logging](file-writing.md)
