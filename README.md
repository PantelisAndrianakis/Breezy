<p align="center"> <img src="https://github.com/PantelisAndrianakis/Breezy/blob/main/logo.png"/></p>

## What is Breezy?

Breezy is a statically-typed, object-oriented language with clean, familiar syntax that compiles **directly to native x86-64 machine code**. No virtual machine. No bytecode. No tracing heap garbage collector. Just your code, lowered to the metal - with memory and concurrency handled automatically by the language.

The promise is simple: **the ergonomics of a managed language, with the footprint and latency characteristics traditionally associated with C.**

---

## Hello, Breezy

```breezy
class Greeter
{
    string name;

    void init(string who)
    {
        name = who;
    }

    void greet()
    {
        print("Hello, " + name + "!");
    }
}

void main()
{
    Greeter g = new Greeter();
    g.init("Breezy");
    g.greet();              // Prints: Hello, Breezy!
}
```

Familiar on purpose. If you have written object-oriented code before, you already know how to read it - and it compiles to a native executable with no separate runtime to install.

---

## Why Breezy is fast - and stays fast

Most high-level languages buy their convenience with a heavier runtime: a JIT that warms up, a GC that pauses, a heap that balloons to 2–5× your live data. Breezy refuses that trade.

| What you get                          | How Breezy delivers it                                                                                                                                             |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 🚀 **Native execution**               | Compiles directly to x86-64 machine code. No interpreter, no bytecode, no JIT warm-up.                                                                  |
| 🪶 **Low memory usage**               | Automatic Reference Counting (ARC) releases objects as soon as they are no longer referenced, keeping memory close to the application's live data.      |
| ⚡ **Predictable latency**             | No stop-the-world heap scans. Reclamation happens incrementally, with a bounded cycle collector only for reference cycles.                              |
| 🧵 **Scalable concurrency**           | Lightweight coroutines ("breezes") multiplex large numbers of concurrent tasks across a small number of operating-system threads.                       |
| 🔌 **Direct native interoperability** | Objects never move in memory, so pointers pass directly to native libraries without pinning or copying.                                                 |
| ✍️ **Automatic resource management**  | Memory and concurrency are handled by the language - no manual deallocation, ownership tracking, or lifetime management in application code.            |


**The headline use case:** Breezy is being built so you can write **high-throughput enterprise software** in it - backend services and APIs that hold tens of thousands of concurrent connections, with predictable low latency (no GC pauses to spike your p99), a tight RAM footprint that cuts your hosting bill, and direct access to native database and crypto libraries. That goal drives every design decision below.

---

# Language Philosophy

## The Principle Behind Every Design Decision

This language exists to remove unnecessary barriers between human-readable code and efficient machine code.

What matters is the ability to express ideas clearly while producing highly optimized native binaries.

Many modern languages claim to improve software development but often introduce additional runtime overhead, unnecessary abstractions, garbage collection, virtual machines, interpreted execution models, or even enforcement for a particular coding style. While these approaches may simplify certain aspects of development, they frequently do so at the expense of performance, predictability, or resource efficiency.

This language takes a different approach.

Its purpose is to allow developers to write straightforward code while relying on the compiler to generate efficient machine code suitable for the most demanding software systems.

## The Real Problems to Solve

After decades of software development, two of the most fundamental challenges are:

* Memory management
* Concurrency

These are infrastructure problems, not business logic problems.

Developers should not be required to manually allocate memory, track object ownership, write synchronization primitives, or coordinate thread execution. Such tasks increase complexity and become a major source of bugs and maintenance costs.

A modern language should solve these problems automatically without sacrificing performance.

## Performance Is Non-Negotiable

This language is intended to compete in domains where efficiency matters.

It should be capable of powering:

* Web browser rendering engines and script runtimes
* Video codec encoders
* Optimizing compiler backends
* Real-time 3D rendering engines
* Relational database engines
* Massive multiplayer game servers
* High-frequency trading systems
* Software network infrastructure
* Distributed consensus systems
* Real-time operating systems

Success means being capable of addressing these demanding domains without sacrificing the language's core principles.

## Automatic Without Hidden Costs

Automation should not come at the expense of performance.

The language aims to provide:

* Automatic memory management
* Automatic concurrency management
* No tracing heap garbage collector
* No manual locking
* No manual allocation
* No stop-the-world runtime pauses
* No unnecessary runtime overhead

The compiler should perform as much analysis and optimization as possible, allowing developers to focus on solving problems rather than managing resources.

## Platform Neutral Performance

Software should achieve comparable performance across operating systems when executed on equivalent hardware.

The language should not favor one platform over another, nor should developers be forced to redesign software to achieve acceptable performance on a different operating system.

## Final Goal

The objective is simple:

Create a language that makes memory management and concurrency largely disappear from application code without relying on a tracing garbage collector, a virtual machine, or a heavyweight runtime.

Developers should be able to write clear, familiar, object-oriented code while the compiler handles the complexity of resource management and execution.

The result should be software that retains the performance, predictability, memory efficiency, and native interoperability traditionally associated with systems programming languages, while offering the productivity expected from modern managed languages.

In short, Breezy aims to deliver the simplicity developers want without requiring the runtime costs they have learned to accept.

---

## Documentation

This README covers *what* Breezy is. For *how to use it* - every language feature, the standard library, and the build tooling, each on its own page - read the full guide:

📖 **[The Breezy Language Guide → documentation/guide.md](documentation/guide.md)**
