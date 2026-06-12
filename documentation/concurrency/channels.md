# Channels

Breezes coordinate by **passing values over channels** rather than sharing mutable memory - *share memory by communicating*. A `channel<T>` is a **bounded buffered** queue: it holds up to a fixed number of values, blocks a sender when full, and blocks a receiver when empty. That blocking is what gives you **backpressure** for free.

← [Back to the guide](../guide.md)

---

## Creating a channel

`new channel<T>(N)` reserves a ring of `N` slots. `N` is the capacity - the number of values the channel can hold before a sender has to wait.

```breezy
channel<int> c = new channel<int>(2);     // Capacity 2.
```

---

## send and recv

`send` adds a value; `recv` takes one out. When the channel is **full**, `send` **parks** the breeze until space frees up. When the channel is **empty**, `recv` parks until a value arrives. A spawned worker can take a channel (and up to four arguments of any type) directly.

```breezy
void producer(channel<int> out)
{
	out.send(10);
	out.send(20);
	out.send(30);            // With capacity 2, this parks until main drains one.
}

void main()
{
	channel<int> c = new channel<int>(2);
	spawn producer(c);

	int total = 0;
	int i;
	for (i = 0; i < 3; i++)
	{
		total += c.recv();   // Parks while the channel is empty.
	}
	print(total);            // 60.
}
```

---

## Backpressure

The bounded capacity is the whole point. If a consumer is slower than its producer, the channel fills, and the next `send` **parks the producer** until the consumer catches up. This applies natural backpressure instead of letting an unbounded queue grow until the server runs out of memory - exactly what you want in a high-throughput service.

---

## Rules & gotchas

- **Capacity is fixed at creation:** `new channel<T>(N)`.
- **`send` parks when full; `recv` parks when empty** - both yield the breeze, not the OS thread.
- **A full channel applies backpressure** to the producer, bounding memory use.
- **Pass channels to spawned workers** as arguments (up to four arguments of any type).
- **Any value can cross a channel safely** - objects, strings, [arrays](../types/arrays.md), [collections](../types/collections.md), and [maps](../types/maps.md) included. The runtime marks the whole object graph shared at the handoff, and shared containers synchronize their operations automatically.
- **Prefer channels over shared mutable state** for cross-breeze communication - sharing is safe, but message passing stays the faster, clearer architecture.

---

← [Breezes](breezes.md) · [Back to the guide](../guide.md) · Next: [The zone model](zone-model.md)
