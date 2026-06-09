# Breezes (Lightweight Coroutines)

A **breeze** is Breezy's unit of concurrency: a lightweight coroutine with its own stack, multiplexed many-to-few onto a small number of operating-system threads (one scheduler thread per CPU core). You write **plain, blocking-looking code**, and the runtime transparently parks a breeze when it waits and resumes it when it can continue. There is **no `async`/`await`, no callbacks, and no function colouring**.

← [Back to the guide](../guide.md)

---

## spawn - start a breeze

`spawn` launches a function as a new breeze. It returns immediately; the new breeze runs concurrently.

```breezy
void worker()
{
	print("Working.");
}

void main()
{
	spawn worker();      // Start worker as its own breeze.
	print("Started.");
}
```

A breeze stack is tens of kilobytes, not the megabytes an OS thread costs. **10,000 concurrent connections is roughly a few hundred MB of stacks**, instead of the tens of GB the same count of OS threads would need.

---

## Blocking-looking I/O

The point of breezes is that I/O code *reads* sequentially while *behaving* asynchronously. A call that looks blocking actually **yields** the breeze until the data is ready, freeing the core to run other breezes meanwhile.

```breezy
void handleClient(Socket sock)
{
	while (true)
	{
		string line = sock.readText(1024);   // Looks blocking - actually parks the breeze.
		sock.writeText(line);
	}
}

void main()
{
	Listener l = Network.listen(7777);
	while (true)
	{
		Socket sock = l.accept();
		spawn handleClient(sock);     // One cheap breeze per connection.
	}
}
```

See [Native I/O](../io/native-io.md) for how the I/O facade parks and resumes breezes.

---

## yield - cooperative scheduling

`yield()` hands control back to the scheduler, which resumes the breeze later. Breezes are **cooperatively** scheduled, with deterministic FIFO interleaving on a single scheduler:

```breezy
void a()
{
	print(1);
	yield();        // Hand off to the scheduler; resumes here later.
	print(3);
}

void b()
{
	print(2);
	yield();
	print(4);
}

void main()        // Breeze 0.
{
	spawn a();
	spawn b();
	yield();
	print(9);
}
// Deterministic FIFO interleaving prints: 1 2 9 3 4
```

---

## How it scales

Breezes are multiplexed M:N onto scheduler threads - one thread per core, with work-stealing across cores. A blocking-looking I/O call parks the breeze instead of the thread, so a single core can keep tens of thousands of connections flowing. Objects that never cross between breezes keep **non-atomic** reference counts on the hot path; only objects that actually move between breezes pay the atomic cost (see the [zone model](zone-model.md)).

---

## Rules & gotchas

- **`spawn fn(args)` starts a breeze** and returns immediately.
- **Breezes are cheap** - tens of KB of stack each; spawn one per connection without worry.
- **No `async`/`await` and no function colouring** - ordinary functions run as breezes.
- **`yield()` cooperatively hands off** to the scheduler.
- **Breezes communicate through [channels](channels.md)** rather than shared mutable state.

---

← [Automatic memory](../memory/automatic-memory.md) · [Back to the guide](../guide.md) · Next: [Channels](channels.md)
