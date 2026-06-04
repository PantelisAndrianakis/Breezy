# File Writing & Logging

Writing to disk well means **not** issuing a syscall per write and **not** blocking your hot path on the disk. Breezy gives you a buffered `FileWriter` for the first and a channel-fed `Logger` for the second.

← [Back to the guide](../guide.md)

---

## FileWriter - buffered writes

A `FileWriter` **buffers** your writes in a userspace buffer, so many small writes coalesce into far fewer syscalls. Each flush runs off the scheduler core (on the [offload pool](native-io.md)), so writing never stalls the breeze's core.

Open a writer with `File.openWrite` (truncate) or `File.openAppend` (append). For one-shot writes where buffering is not needed, the simpler [`File` namespace](../stdlib/file.md) functions (`writeText`, `appendText`) are enough.

---

## Logger - logging without touching the hot path

Logging should never make a request wait on disk. A `Logger` owns a dedicated **logger breeze** that drains a `channel<string>` and writes off the hot path. Calling `log(...)` just hands a string over the channel and returns **immediately** - the actual disk write happens on the logger breeze, so a tick never waits on disk.

```breezy
Logger log;
log = Log.open("game.log");          // Owns a channel + a FileWriter + a logger breeze.
log.log("Tick " + n + " done.");     // Hands off over the channel - returns at once.
// ... many ticks later ...
log.close();                          // Drain, flush, and join the logger breeze.
```

- `Log.open(path)` returns a `Logger` wired to its own channel, `FileWriter`, and logger breeze.
- `log.log(message)` enqueues a line and returns immediately.
- `log.close()` drains the channel, flushes the buffer, and joins the logger breeze - call it before exit so nothing is lost.

This is the [channel](../concurrency/channels.md) pattern applied to I/O: the producer (your game loop) never blocks on the consumer (the disk).

---

## Rules & gotchas

- **`FileWriter` buffers** - small writes coalesce and flush in fewer syscalls, off the scheduler core.
- **`Logger.log` returns immediately** - the write happens on the logger breeze, never on your hot path.
- **Always `close()` a `Logger`** before exit to drain and flush pending lines.
- **For simple one-shot writes**, the [`File`](../stdlib/file.md) functions are simpler than a `FileWriter`.

---

← [Native I/O](native-io.md) · [Back to the guide](../guide.md) · Next: [C library interop](../ffi/c-interop.md)
