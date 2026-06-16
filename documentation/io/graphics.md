# Pixel Surface - A Window You Draw Into

`Graphics` opens a native **pixel surface**: a resizable window you hand a raw
framebuffer to, one frame at a time. You fill an `int[]` of ARGB pixels and call
`present`; input arrives by polling. This is the surface a software renderer - a
game, an emulator, a visualizer - draws into.

It is backed by **SDL2, loaded dynamically at run time** - so a program links with no
SDL dependency, and a program that never references `Graphics`/`Surface` carries no
graphics code at all. If SDL2 (or a display) is absent, the first `Graphics` call
throws a catchable [`IOException`](../stdlib/file.md).

← [Back to the guide](../guide.md)

---

## A render loop

Every call looks blocking; underneath, all SDL work runs on one dedicated render
thread, so your breeze hands frames over and reads input without ever stalling the
scheduler.

```breezy
void main()
{
	Surface s = Graphics.open(640, 400, "My Renderer");   // Window + render thread.
	int[] fb = new int[640 * 400];                        // ARGB8888, one int per pixel.

	while (s.isOpen())
	{
		long e = s.pollEvent();
		while (e != 0)                 // Drain every queued event this frame.
		{
			handle(e);
			e = s.pollEvent();
		}

		render(fb);                    // Fill the framebuffer.
		s.present(fb);                 // Hand it to the window. Non-blocking.
		System.sleep(28);              // Pace the loop yourself (~35 fps).
	}
	s.close();
}
```

`present` does **not** wait for the display refresh (vsync is off), so it never blocks
a scheduler core - you set the pace with [`System.sleep`](native-io.md).

---

## The API

| Call | Returns | Behaviour |
| --- | --- | --- |
| `Graphics.open(width, height, title)` | `Surface` | Open the window. Throws `IOException` if SDL2 or a display is unavailable. |
| `s.present(int[] pixels)` | - | Blit a `width*height` ARGB framebuffer and show it. Throws if the length is wrong or the device is lost. |
| `s.pollEvent()` | `long` | The next queued input event, packed (see below); `0` when none remain this frame. Non-blocking. |
| `s.isOpen()` | `bool` | `false` once the user closes the window. |
| `s.close()` | - | Stop the render thread and destroy the window. Idempotent. |

The pixel format is **ARGB8888** - one `int` per pixel, alpha in the high byte. Because
Breezy's `int` is 32-bit signed, build an opaque colour with a shift rather than a
literal: `(255 << 24) | (r << 16) | (g << 8) | b`.

---

## Events

`pollEvent` returns a packed `long` (the same packed-word style as
[`System.pollMouse`](native-io.md)). Decode it as:

```
kind = (e >> 48) & 0xFFFF
a    = (e >> 24) & 0xFFFFFF
b    =  e        & 0xFFFFFF
```

| `kind` | Meaning | `a` | `b` |
| --- | --- | --- | --- |
| 1 | key down | key code | - |
| 2 | key up | key code | - |
| 3 | mouse move | x | y |
| 4 | mouse button down | x | button |
| 6 | window closed (quit) | - | - |

Key codes are SDL keycodes, passed through unmapped. A `kind == 6` quit event also
flips `isOpen()` to `false`, so a loop guarded by `while (s.isOpen())` exits cleanly on
the window-close button.

---

## OpenGL - a hardware 3D context

For accelerated 3D, `Graphics.openGL` opens an **OpenGL window** instead of a
software framebuffer. You then call OpenGL **directly from Breezy** through
[`extern dynamic`](../ffi/c-interop.md) - `openGL` installs an
`SDL_GL_GetProcAddress` resolver, so a declared `extern dynamic` GL function
binds to the live driver on first call.

```breezy
extern dynamic void glClear(int mask);
extern dynamic void glClearColor(float r, float g, float b, float a);
extern dynamic void glBegin(int mode);
extern dynamic void glColor3f(float r, float g, float b);
extern dynamic void glVertex2f(float x, float y);
extern dynamic void glEnd();

void main()
{
	GlSurface gs = Graphics.openGL(640, 480, "Breezy GL");
	for (int frame = 0; frame < 600; frame = frame + 1)
	{
		long e = gs.pollEvent();           // Drain input (same packed events as above).
		glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
		glClear(0x4000);                   // GL_COLOR_BUFFER_BIT
		glBegin(0x0004);                   // GL_TRIANGLES
		glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(0.0f, 0.5f);
		glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(-0.5f, -0.5f);
		glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(0.5f, -0.5f);
		glEnd();
		gs.swapBuffers();                  // Present the frame.
	}
	gs.close();
}
```

The example uses legacy immediate mode for brevity; a modern core/shader pipeline
(VAOs, VBOs, `glDrawArrays`) works through the **identical** `extern dynamic` path -
declare the entry points you need and call them.

| Call | Returns | Behaviour |
| --- | --- | --- |
| `Graphics.openGL(width, height, title)` | `GlSurface` | Open a GL window + context. Throws `IOException` if SDL2, OpenGL, or a display is unavailable. |
| `gs.pollEvent()` | `long` | Next packed input event (same encoding as `Surface`); `0` when none remain. |
| `gs.swapBuffers()` | - | Present the rendered frame (`SDL_GL_SwapWindow`). |
| `gs.isOpen()` | `bool` | `false` once you `close()`; watch `pollEvent` for the `kind == 6` quit event to break your loop. |
| `gs.close()` | - | Destroy the context and window. Idempotent. |

### The one hard rule: never yield inside a GL loop

A GL context is bound to **one OS thread**. The breeze that calls `openGL` owns the
context on its scheduler worker, and a *running* breeze never migrates - so the render
loop stays on that worker **as long as it never yields**. Do **not** `await`,
`System.sleep`, or join a spawned breeze anywhere between `openGL` and `close`: a yield
can move the breeze to another worker, off the context's thread. `swapBuffers` detects
this and aborts with a clear message rather than corrupting the driver. Pace the loop
with the display's own vsync, not `System.sleep`.

One more: the `extern dynamic` resolver is global and last-writer-wins, so call
`openGL` **after** any [`Ffi.bind`](../ffi/c-interop.md) - otherwise the GL resolver
would be replaced and `gl*` lookups would miss.

---

## Rules & gotchas

- **`Graphics.open` needs SDL2 at run time** (loaded dynamically) and a display; it
  throws `IOException` when either is absent - guard or catch it for headless/CI runs.
- **One `int` per pixel, ARGB8888**, alpha in the high byte; build opaque colours with
  `(255 << 24) | ...` because `int` is 32-bit signed.
- **`present` is non-blocking** (vsync off). Pace your loop with `System.sleep`; don't
  busy-spin.
- **`present`'s framebuffer length must equal `width*height`** or it throws.
- **Input is poll-only** - drain `pollEvent` until it returns `0` each frame.
- **For GPU / 3D, use `Graphics.openGL`** (see [OpenGL](#opengl---a-hardware-3d-context)
  above) - a real OpenGL context you drive with `extern dynamic` `gl*` calls. The
  software `Surface` here stays a CPU framebuffer.
- **Never yield inside a GL render loop** - the context is thread-bound; `await`/`sleep`
  between `openGL` and `close` can migrate the breeze and `swapBuffers` will abort.

---

← [Native I/O](native-io.md) · [Back to the guide](../guide.md)
