# File

`File` is a **static namespace** for filesystem work - creating, reading, writing, searching, and inspecting files and folders. Predicates (`exists`, `isFile`, `isFolder`) return a `bool`; everything else **throws an `IOException`** on failure, so errors surface where they happen instead of silently corrupting state.

← [Back to the guide](../guide.md)

---

## Creating, writing, reading

```breezy
File.createFolder("data/logs");                    // Like mkdir -p: makes intermediate folders.
File.writeText("data/note.txt", "hello\nworld\n");
File.appendText("data/note.txt", "again\n");

print(File.exists("data/note.txt"));               // true.
print(File.readText("data/note.txt"));             // The file's contents.
// Reading a structured document? Pass the text to Xml.parse -- see [XML](xml.md).

foreach (string line in File.readLines("data/note.txt"))
{
	print(line);
}
```

---

## Searching

`search` and `searchRecursive` take a glob (`*` and `?`) and return a `string[]` of full paths:

```breezy
foreach (string p in File.search("data", "*.txt"))   // In-folder.
{
	print(p);
}

string[] all = File.searchRecursive("data", "*");             // Whole tree.
```

`list(folder)` returns the entries of a folder.

---

## Binary I/O

```breezy
byte[] bytes = File.readBytes("data/note.txt");
File.writeBytes("data/copy.bin", bytes);
```

---

## Random access: `FileChannel`

For random-access storage (a database file, a consensus log), open a
`FileChannel` and read/write at explicit byte offsets without moving a cursor:

```breezy
FileChannel c = File.openChannel("store.dat");

byte[] rec = new byte[4];
rec[0] = (byte)1; rec[1] = (byte)2; rec[2] = (byte)3; rec[3] = (byte)4;

if (c.lock())                       // Exclusive advisory whole-file lock.
{
	c.writeAt(0, rec);              // Positioned write (pwrite); no cursor.
	c.sync();                       // Durability barrier.
	byte[] back = c.readAt(0, 4);   // Positioned read (pread).
	c.unlock();
}
c.close();
```

- `readAt(offset, maxBytes) -> byte[]` - positioned read; owned `byte[]` of the
  bytes actually read (shorter at EOF, empty past EOF).
- `writeAt(offset, data) -> int` - positioned write of a `byte[]`; returns the
  byte count.
- `readInto(buf, offset, maxLen) -> int` - positioned read into a reusable
  `byte[]`; returns the count.
- `size() -> long` / `truncate(size)` - query / set the file length.
- `sync()` - flush buffers to disk (durability barrier).
- `lock() -> bool` - acquire an **exclusive advisory** lock on the whole file,
  parking the breeze until granted; returns `true` once held, `false` on failure.
- `unlock()` - release the advisory lock (best-effort).
- `close()` - close the handle.

`writeAt` + `sync` + `lock` together give a crash-safe, single-writer storage
primitive. The lock is **advisory** (POSIX `flock` / Windows `LockFileEx`): it
coordinates cooperating processes that all lock, and does not block unrelated
readers.

---

## Memory-mapped files: `MappedFile`

For zero-copy access to a large file - scanning, random reads, in-place edits -
map it into memory with `FileChannel.mmap()`. Reads and writes then hit the OS
page cache directly, with **no `read()`/`write()` syscall per access**.

```breezy
FileChannel c = File.openChannel("data.bin");
c.truncate(1 << 20);                // The file must be non-empty to map.

MappedFile m = c.mmap();            // Whole-file read-write map (MAP_SHARED).
m.putLong(0, 0x1234567);           // Write straight into the mapped pages.
long v = m.getLong(0);             // Read with no syscall.
m.flush();                         // Force dirty pages to disk.
m.close();                         // Unmap.
c.close();
```

- `c.mmap() -> MappedFile` - map the whole file read-write. Throws
  [`IOException`](#deleting-and-handling-errors) if the file is empty (truncate
  it to a size first) or cannot be mapped.
- `size() -> long` - the mapped length in bytes.
- `getByte(off) -> int` (0..255), `getInt(off) -> int`, `getLong(off) -> long` -
  read a native-endian value at a byte offset.
- `putByte(off, v)`, `putInt(off, v)`, `putLong(off, v)` - write one.
- `copyInto(byte[] dst, srcOffset, len)` - bulk-copy mapped bytes into a `byte[]`
  off the page cache (no `read()` syscall).
- `flush()` - force dirty pages to disk (`msync` / `FlushViewOfFile` +
  `FlushFileBuffers`); parks the breeze on the offload pool. Throws on error.
- `close()` - unmap and release; idempotent.

Offsets are **bounds-checked** - an out-of-range index aborts with the same
message as an array subscript. Values are **native-endian**: a file written and
read on the same architecture round-trips exactly.

### Anonymous memory: `Memory.alloc`

`Memory.alloc(bytes)` returns a `MappedFile` backed by **anonymous memory** - a
zero-filled, page-aligned region not tied to any file. It is the same object with
the same accessors, useful as a raw scratch buffer or arena:

```breezy
MappedFile m = Memory.alloc(64);   // 64 zeroed bytes.
m.putInt(0, 123);
print(m.getInt(0));                // 123
m.close();                         // Release (munmap / UnmapViewOfFile).
```

- `Memory.alloc(bytes) -> MappedFile` - a zero-filled anonymous region of
  `bytes` bytes (must be positive). `flush()` is a no-op (there is no file).

**Concurrency:** mapped bytes are not ARC objects, so overlapping writes from
multiple breezes are **not** auto-synchronized the way a shared container is.
Use a single writer, or coordinate with your own lock / `FileChannel.lock()`.

**Durability:** call `flush()` before you rely on the bytes being on disk;
`close()` (and process exit) unmaps but does not guarantee a flush.

---

## Windows attributes

Read-only, hidden, system, and archive flags via the `File.READONLY` / `File.HIDDEN` / `File.SYSTEM` / `File.ARCHIVE` constants:

```breezy
File.setAttribute("data/note.txt", File.READONLY, true);
print(File.hasAttribute("data/note.txt", File.READONLY));   // true.
File.setAttribute("data/note.txt", File.READONLY, false);
```

---

## Deleting, and handling errors

Failed operations throw an [`IOException`](../language/exceptions.md) you can catch:

```breezy
try
{
	File.delete("data/missing.txt");
}
catch (IOException e)
{
	print("Delete failed.");
}

File.deleteRecursive("data");             // Remove the whole tree.
```

---

## The full surface

- **Create / delete:** `createFile`, `createFolder` (makes intermediate folders), `delete` (a file or empty folder), `deleteRecursive` (a folder tree).
- **Read / write text:** `readText`, `readLines`, `writeText`, `appendText`.
- **Read / write binary:** `readBytes`, `writeBytes` (over `byte[]`).
- **Search:** `list(folder)`, `search(folder, glob)`, `searchRecursive(folder, glob)` -> `string[]` of full paths.
- **Attributes:** `setAttribute(path, attr, on)`, `hasAttribute(path, attr)` with the `READONLY` / `HIDDEN` / `SYSTEM` / `ARCHIVE` constants.
- **Predicates:** `exists`, `isFile`, `isFolder` -> `bool`.

---

## A note on concurrency

The **data** operations (the reads and writes) run on the [offload pool](../io/native-io.md): called inside a breeze, they park it while a worker does the blocking syscall, so the scheduler core never stalls - the blocking-looking surface is unchanged. **Metadata** operations (exists, create, delete, search, attributes) are synchronous.

---

## Rules & gotchas

- **Predicates return `bool`; everything else throws `IOException`** on failure - wrap risky calls in `try`/`catch`.
- **`createFolder` makes intermediate folders** (like `mkdir -p`).
- **Search returns full paths**; globs use `*` and `?`.
- **Attributes are Windows concepts** - `HIDDEN`/`SYSTEM`/`ARCHIVE` have no POSIX equivalent.
- **Data ops offload; metadata ops are synchronous.**
- **`FileChannel.lock()` is an advisory whole-file lock** - it coordinates processes that cooperate by locking; it returns `false` on failure rather than throwing, and `unlock()` is best-effort. The blocking acquire parks the breeze on the offload pool.

---

← [Regex](regex.md) · [Back to the guide](../guide.md) · Next: [System](system.md)
