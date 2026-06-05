<p align="center"> <img src="https://github.com/PantelisAndrianakis/Breezy/blob/main/logo.png"/></p>

## What is Breezy?

Breezy is a statically-typed, object-oriented language with clean, familiar syntax that compiles **straight to native x86-64 assembly**. No virtual machine. No bytecode. No tracing garbage collector. Just your code, lowered to the metal - with memory and concurrency handled *for* you, automatically.

The promise is simple: **the ergonomics of a managed language, the footprint and latency of C.**

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
    Greeter g;
    g = new Greeter();
    g.init("Breezy");
    g.greet();              // Prints: Hello, Breezy!
}
```

Familiar on purpose. If you've written Java or C#, you already know how to read it - and it compiles straight to a native executable with no runtime to ship.

---

## Why Breezy is fast - and stays fast

Most "easy" languages buy their convenience with a heavy runtime: a JIT that warms up, a GC that pauses, a heap that balloons to 2–5× your live data. Breezy refuses that trade.

| What you get                          | How Breezy delivers it                                                                                                                                             |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 🚀 **Native execution**               | Compiles directly to x86-64 assembly. No interpreter, no bytecode, and no JIT compilation.                                                                         |
| 🪶 **Low memory usage**               | Automatic Reference Counting (ARC) releases objects when their reference count reaches zero, keeping memory usage close to the application's live data.            |
| ⚡ **Predictable latency**             | No tracing garbage collector and no stop-the-world collection cycles. Memory reclamation occurs incrementally as objects become unreachable.                       |
| 🧵 **Scalable concurrency**           | Lightweight coroutines ("breezes") allow large numbers of concurrent tasks to be multiplexed across a small number of operating-system threads.                    |
| 🔌 **Direct native interoperability** | Objects are never relocated in memory, allowing pointers to be passed directly to C libraries without pinning or object movement.                                  |
| ✍️ **Automatic resource management**  | Memory management and concurrency are handled by the language, eliminating manual deallocation, ownership tracking, and lifetime management from application code. |


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

After decades of software development, only a small number of challenges consistently prevent developers from writing simple and maintainable code:

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

If the language cannot be used effectively in these environments, then it has failed one of its primary objectives.

## Automatic Without Hidden Costs

Automation should not come at the expense of performance.

The language aims to provide:

* Automatic memory management
* Automatic concurrency management
* No garbage collector
* No manual locking
* No manual allocation
* No unpredictable runtime pauses
* No unnecessary runtime overhead

The compiler should perform as much analysis and optimization as possible, allowing developers to focus on solving problems rather than managing resources.

## Platform Neutral Performance

Software should achieve comparable performance across operating systems when executed on equivalent hardware.

The language should not favor one platform over another, nor should developers be forced to redesign software to achieve acceptable performance on a different operating system.

## Final Goal

The objective is simple:

Create a language that makes memory management and concurrency largely disappear from application code without relying on a garbage collector, a virtual machine, or a heavyweight runtime.

Developers should be able to write clear, familiar, object-oriented code while the compiler handles the complexity of resource management and execution.

The result should be software that retains the performance, predictability, memory efficiency, and native interoperability traditionally associated with systems programming languages, while offering the productivity expected from modern managed languages.

In short, Breezy aims to deliver the simplicity developers want without requiring the runtime costs they have learned to accept.

---

## Documentation

This README covers *what* Breezy is. For *how to use it* - every language feature, the standard library, and the build tooling, each on its own page - read the full guide:

📖 **[The Breezy Language Guide → documentation/guide.md](documentation/guide.md)**
