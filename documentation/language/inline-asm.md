# Inline assembly

An `asm` block emits raw x86-64 assembly **verbatim** into the generated code. It
is an escape hatch for the rare cases a compiler cannot express - a specific
instruction, a syscall, a hardware feature - and it trades all of Breezy's safety
for direct control.

← [Back to the guide](../guide.md)

---

## Form

`asm { ... }` with one string literal per line. Each string is written into the
output assembly exactly as given:

```breezy
asm
{
	"push rax";
	"xor rax, rax";
	"pop rax";
}
```

The assembler is NASM-syntax (Intel, `dest, src`).

---

## You own the machine state

Inside an `asm` block there is **no checking and no help**. The compiler does not
know what your instructions read or write, so:

- **Preserve what you clobber.** Save and restore any register the surrounding
  code may rely on (callee-saved registers, `rbp`, `rsp`, and the frame). The
  example above is balanced - it restores `rax` before leaving.
- **Do not assume a register holds a particular value** on entry; the compiler's
  allocation around the block is not part of the contract.
- **Labels are file-global.** A label you define shares the program's namespace.

This is a deliberately small, unguarded primitive. A typed `asm` with declared
inputs, outputs, and clobbers is a possible future addition; until then, treat a
block as a black box you are fully responsible for.

---

## Rules & gotchas

- **One string literal per line**, NASM syntax, emitted verbatim.
- **Register and frame discipline are yours** - an unbalanced block corrupts the
  caller.
- **Greppable by design** - every use is an explicit `asm` block.

---

← [Back to the guide](../guide.md)
