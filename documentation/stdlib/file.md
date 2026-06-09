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

---

← [Regex](regex.md) · [Back to the guide](../guide.md) · Next: [System](system.md)
