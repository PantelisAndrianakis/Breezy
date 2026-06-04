# The Zone Model

The zone model is Breezy's **recommended architecture** for a concurrent service. You shard your state into **zones** - per tenant, account, region, or whatever boundary fits your domain - and give each zone to a single breeze that **exclusively owns its data**. Inside a zone there are **no locks**; across zones, breezes talk through [channels](channels.md). This scales across all your cores with near-zero contention.

← [Back to the guide](../guide.md)

---

## The idea

Most concurrency bugs and most contention come from many threads touching the same mutable data behind locks. The zone model removes the shared mutable data instead of guarding it:

- **Partition state into zones.** Each zone is a self-contained slice of your world.
- **One breeze owns each zone.** Only that breeze ever touches the zone's data.
- **No locks inside a zone.** Because a single breeze owns the data, there is nothing to race against.
- **Cross-zone interaction goes through channels.** A breeze that needs another zone's data sends it a message rather than reaching in.

---

## Why it is fast

Scaling by zone means the work spreads across all your cores with **near-zero contention** - zones rarely wait on each other, and never on a shared lock. It also unlocks a key optimisation in the memory system: because most objects stay within one zone (one breeze), the compiler keeps their [reference counts](../memory/automatic-memory.md) **non-atomic** on the hot path, paying the more expensive atomic operations **only** for the objects that actually cross between breezes.

So the architecture and the memory model reinforce each other: design around zones, and you get both lock-free clarity and the cheapest possible reference counting.

---

## Putting it together

The pieces from the previous pages combine into the recommended shape of a Breezy service:

- [Breezes](breezes.md) give you cheap concurrency - one breeze per zone (and often one per connection).
- [Channels](channels.md) carry cross-zone messages with built-in backpressure.
- [Automatic memory](../memory/automatic-memory.md) reclaims everything deterministically, cheaply within a zone.

---

## Rules & gotchas

- **One breeze owns each zone's data** - never share a zone's mutable state across breezes.
- **No locks inside a zone** - ownership replaces locking.
- **Cross-zone communication is by channel**, not by direct access.
- **Keeping objects zone-local keeps their refcounts non-atomic** - cross-breeze sharing is what costs more, so share deliberately.

---

← [Channels](channels.md) · [Back to the guide](../guide.md) · Next: [Native I/O](../io/native-io.md)
