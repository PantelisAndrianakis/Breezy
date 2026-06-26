# Desktop

`Desktop` is the entry point to Breezy's **GUI toolkit**: native desktop windows, the common
controls, and a Swing-style listener model, running on the same source on **Windows** and
**Linux (incl. KDE)**. It is backed by GTK3, which the runtime **loads dynamically at run
time** - so a program links with no GTK dependency, and a program that never references
`Desktop` neither reserves the widget names nor carries any GUI code.

← [Back to the guide](../guide.md)

---

## Is a desktop available?

`Desktop.isEnabled()` reports whether a graphical display is available: it
returns `true` only when GTK can be loaded **and** a display exists (an X11/Wayland server on
Linux, an interactive session on Windows). Guard GUI code with it so the same program runs
safely on a headless server or in CI.

```breezy
if (Desktop.isEnabled())
{
	Frame frame = new Frame();
	// ... build the UI ...
}
else
{
	print("No desktop available.");
}
```

Constructing a widget when the desktop is unavailable is unsupported - always guard with
`isEnabled()` first.

---

## The widget hierarchy

Every widget descends from `Component`. Containers hold children; windows are top-level.

| Class | Extends | Purpose |
| --- | --- | --- |
| `Component` | - | Base of every widget: `setVisible(bool)`, `setEnabled(bool)`. |
| `Container` | `Component` | Holds children: `add(Component)`. |
| `Window` | `Container` | Top-level: `setSize(int, int)`, `show()`, `dispose()`. |
| `Frame` | `Window` | Titled main window (see below). |
| `Panel` | `Container` | A grouping surface. |
| `Label` | `Component` | Static text: `setText(string)`. |
| `Button` | `Component` | A push button: `setText(string)`, `addActionListener(ActionListener)`. |

`Frame` adds `setTitle(string)`, `addWindowListener(WindowListener)`, and two ways to place
children:

- `frame.add(component)` - places the child in the center.
- `frame.addRegion(component, region)` - places it in a `BorderLayout` region.

```breezy
Frame frame = new Frame();
frame.setTitle("My App");
frame.setSize(400, 300);

Label header = new Label();
header.setText("Welcome");
frame.addRegion(header, BorderLayout.NORTH);

Panel body = new Panel();
frame.add(body);              // Center.

frame.show();
```

`BorderLayout` provides the region constants `NORTH`, `SOUTH`, `WEST`, `EAST`, and `CENTER`.

---

## Events: listeners and `Desktop.run()`

Breezy has no callbacks, so events are delivered through **interfaces** you implement, exactly
like Swing. You register a listener on a widget, then call `Desktop.run()` once: it pulls
native events and dispatches each to the matching listener - **on a normal breeze**, so your
listener code can allocate, spawn, do I/O, and call back into the UI freely.

```breezy
class Greeter implements ActionListener
{
	void actionPerformed(ActionEvent e)
	{
		print("Button clicked.");
	}
}

void main()
{
	if (not Desktop.isEnabled())
	{
		return;
	}

	Frame frame = new Frame();
	frame.setTitle("Hello");
	frame.setSize(300, 150);

	Button button = new Button();
	button.setText("Click me");
	button.addActionListener(new Greeter());
	frame.add(button);

	frame.show();
	Desktop.run();                // Dispatches events until the last window closes.
	print("Done.");
}
```

The listener interfaces:

| Interface | Method | Fires on |
| --- | --- | --- |
| `ActionListener` | `actionPerformed(ActionEvent e)` | A `Button` click. |
| `WindowListener` | `windowClosing(WindowEvent e)` | A `Frame` closing. |

Each event carries its originating widget as `e.source`. `Desktop.run()` returns when the last
top-level window is closed.

---

## How it runs

- **GTK runs on its own thread.** Every UI call is marshalled onto a single dedicated GTK
  thread; the calling breeze **parks** (like any `blocking` call) until it completes, so a UI
  call never stalls a scheduler core. Your listener code never runs on the GTK thread.
- **GTK is loaded dynamically.** No GTK is needed to *link* a program. To *run* a GUI program
  the GTK3 runtime must be present:
  - **Linux (KDE/GNOME):** `libgtk-3-0`, normally already installed.
  - **Windows:** an MSYS2 GTK3 runtime on `PATH` (`libgtk-3-0.dll` and its dependencies).
- **Opt-in names.** The widget class names (`Frame`, `Button`, `Panel`, ...) are reserved only
  in programs that reference `Desktop`. A program that never mentions `Desktop` is completely
  unaffected and keeps those names free.

---

## Rules & gotchas

- **Guard with `Desktop.isEnabled()`** before constructing any widget; it reports headless
  environments cleanly.
- **Call `Desktop.run()` once** after building the UI; it parks and dispatches until the last
  window closes, then returns.
- **Listeners are interfaces** - implement `ActionListener` / `WindowListener` in a class and
  register the instance; there are no lambdas.
- **UI calls park the breeze** (they marshal to the GTK thread); they are not hot paths, so
  this is free in practice.
- **Run-time GTK requirement:** linking needs no GTK, but running a GUI program does - install
  the GTK3 runtime on the target.

---

← [System](system.md) · [Back to the guide](../guide.md)
