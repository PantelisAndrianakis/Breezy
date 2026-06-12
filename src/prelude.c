#include "prelude.h"

/* Built-in vector types, written in Breezy and compiled into every program.
   They are ordinary classes: escape analysis stack-allocates non-escaping
   locals (no heap/ARC), ARC handles escapes, and calculateDistance is just
   Math.sqrt. Distance precision matches the element type (float for *f, else
   double); equals is exact component-wise comparison. */

static const char VEC2I[] =
	"class Vector2i\n"
	"{\n"
	"	int x;\n"
	"	int y;\n"
	"	Vector2i(int x = 0, int y = 0) { this.x = x; this.y = y; }\n"
	"	bool equals(Vector2i o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector2i o)\n"
	"	{\n"
	"		int dx = this.x - o.x;\n"
	"		int dy = this.y - o.y;\n"
	"		return Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC2L[] =
	"class Vector2l\n"
	"{\n"
	"	long x;\n"
	"	long y;\n"
	"	Vector2l(long x = 0L, long y = 0L) { this.x = x; this.y = y; }\n"
	"	bool equals(Vector2l o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector2l o)\n"
	"	{\n"
	"		long dx = this.x - o.x;\n"
	"		long dy = this.y - o.y;\n"
	"		return Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC2F[] =
	"class Vector2f\n"
	"{\n"
	"	float x;\n"
	"	float y;\n"
	"	Vector2f(float x = 0.0f, float y = 0.0f) { this.x = x; this.y = y; }\n"
	"	bool equals(Vector2f o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	float calculateDistance(Vector2f o)\n"
	"	{\n"
	"		float dx = this.x - o.x;\n"
	"		float dy = this.y - o.y;\n"
	"		return (float)Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC2D[] =
	"class Vector2d\n"
	"{\n"
	"	double x;\n"
	"	double y;\n"
	"	Vector2d(double x = 0.0, double y = 0.0) { this.x = x; this.y = y; }\n"
	"	bool equals(Vector2d o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector2d o)\n"
	"	{\n"
	"		double dx = this.x - o.x;\n"
	"		double dy = this.y - o.y;\n"
	"		return Math.sqrt(dx * dx + dy * dy);\n"
	"	}\n"
	"}\n";

static const char VEC3I[] =
	"class Vector3i\n"
	"{\n"
	"	int x;\n"
	"	int y;\n"
	"	int z;\n"
	"	Vector3i(int x = 0, int y = 0, int z = 0) { this.x = x; this.y = y; this.z = z; }\n"
	"	bool equals(Vector3i o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		if (this.z != o.z) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector3i o)\n"
	"	{\n"
	"		int dx = this.x - o.x;\n"
	"		int dy = this.y - o.y;\n"
	"		int dz = this.z - o.z;\n"
	"		return Math.sqrt(dx * dx + dy * dy + dz * dz);\n"
	"	}\n"
	"}\n";

static const char VEC3L[] =
	"class Vector3l\n"
	"{\n"
	"	long x;\n"
	"	long y;\n"
	"	long z;\n"
	"	Vector3l(long x = 0L, long y = 0L, long z = 0L) { this.x = x; this.y = y; this.z = z; }\n"
	"	bool equals(Vector3l o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		if (this.z != o.z) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector3l o)\n"
	"	{\n"
	"		long dx = this.x - o.x;\n"
	"		long dy = this.y - o.y;\n"
	"		long dz = this.z - o.z;\n"
	"		return Math.sqrt(dx * dx + dy * dy + dz * dz);\n"
	"	}\n"
	"}\n";

static const char VEC3F[] =
	"class Vector3f\n"
	"{\n"
	"	float x;\n"
	"	float y;\n"
	"	float z;\n"
	"	Vector3f(float x = 0.0f, float y = 0.0f, float z = 0.0f) { this.x = x; this.y = y; this.z = z; }\n"
	"	bool equals(Vector3f o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		if (this.z != o.z) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	float calculateDistance(Vector3f o)\n"
	"	{\n"
	"		float dx = this.x - o.x;\n"
	"		float dy = this.y - o.y;\n"
	"		float dz = this.z - o.z;\n"
	"		return (float)Math.sqrt(dx * dx + dy * dy + dz * dz);\n"
	"	}\n"
	"}\n";

static const char VEC3D[] =
	"class Vector3d\n"
	"{\n"
	"	double x;\n"
	"	double y;\n"
	"	double z;\n"
	"	Vector3d(double x = 0.0, double y = 0.0, double z = 0.0) { this.x = x; this.y = y; this.z = z; }\n"
	"	bool equals(Vector3d o)\n"
	"	{\n"
	"		if (this.x != o.x) { return false; }\n"
	"		if (this.y != o.y) { return false; }\n"
	"		if (this.z != o.z) { return false; }\n"
	"		return true;\n"
	"	}\n"
	"	double calculateDistance(Vector3d o)\n"
	"	{\n"
	"		double dx = this.x - o.x;\n"
	"		double dy = this.y - o.y;\n"
	"		double dz = this.z - o.z;\n"
	"		return Math.sqrt(dx * dx + dy * dy + dz * dz);\n"
	"	}\n"
	"}\n";

const char *BZY_PRELUDE[] = { VEC2I, VEC2L, VEC2F, VEC2D, VEC3I, VEC3L, VEC3F, VEC3D };
const int   BZY_PRELUDE_COUNT = 8;

/* ---- Desktop GUI prelude (Breezy source). ---- */

/* The C entry points implemented in runtime/desktop.c. Marshalled to the GTK
   thread; marked `blocking` so the calling breeze parks instead of stalling a
   scheduler core while the GTK thread services the request. Names match the C
   symbols (bzy_desktop_*); Breezy `long` carries handles, `int`/`bool` are
   32-bit, `string` marshals to char*. */
/* Split across units of <=8 functions each (the per-unit top-level function
   cap), since each prelude string is parsed as one unit. */
static const char DESKTOP_EXTERNS_A[] =
	"extern blocking bool bzy_desktop_is_enabled();\n"
	"extern blocking long bzy_desktop_frame_new();\n"
	"extern blocking long bzy_desktop_frame_content(long frame);\n"
	"extern blocking void bzy_desktop_frame_set_title(long frame, string title);\n"
	"extern blocking long bzy_desktop_panel_new();\n"
	"extern blocking long bzy_desktop_button_new();\n"
	"extern blocking void bzy_desktop_button_set_text(long btn, string text);\n"
	"extern blocking long bzy_desktop_label_new();\n";

static const char DESKTOP_EXTERNS_B[] =
	"extern blocking void bzy_desktop_label_set_text(long lbl, string text);\n"
	"extern blocking void bzy_desktop_container_add(long parent, long child);\n"
	"extern blocking void bzy_desktop_border_add(long border, long child, int region);\n"
	"extern blocking void bzy_desktop_set_visible(long widget, bool visible);\n"
	"extern blocking void bzy_desktop_set_enabled(long widget, bool enabled);\n"
	"extern blocking void bzy_desktop_window_set_size(long window, int w, int h);\n"
	"extern blocking void bzy_desktop_window_show(long window);\n"
	"extern blocking void bzy_desktop_window_dispose(long window);\n";

static const char DESKTOP_EXTERNS_C[] =
	"extern blocking void bzy_desktop_listen_action(long widget, int regIndex);\n"
	"extern blocking void bzy_desktop_listen_window_close(long window, int regIndex);\n"
	"extern blocking int bzy_desktop_next_event();\n"
	"extern blocking int bzy_desktop_event_kind();\n";

static const char DESKTOP_LISTENERS[] =
	"interface ActionListener\n"
	"{\n"
	"	void actionPerformed(ActionEvent e);\n"
	"}\n"
	"interface WindowListener\n"
	"{\n"
	"	void windowClosing(WindowEvent e);\n"
	"}\n";

static const char DESKTOP_EVENTS[] =
	"class ActionEvent\n"
	"{\n"
	"	Component source;\n"
	"	ActionEvent(Component source) { this.source = source; }\n"
	"}\n"
	"class WindowEvent\n"
	"{\n"
	"	Component source;\n"
	"	WindowEvent(Component source) { this.source = source; }\n"
	"}\n";

static const char DESKTOP_BORDERLAYOUT[] =
	"static class BorderLayout\n"
	"{\n"
	"	static int NORTH = 0;\n"
	"	static int SOUTH = 1;\n"
	"	static int WEST = 2;\n"
	"	static int EAST = 3;\n"
	"	static int CENTER = 4;\n"
	"}\n";

static const char DESKTOP_COMPONENT[] =
	"class Component\n"
	"{\n"
	"	long handle;\n"
	"	int regIndex;\n"
	"	void setVisible(bool v) { bzy_desktop_set_visible(this.handle, v); }\n"
	"	void setEnabled(bool v) { bzy_desktop_set_enabled(this.handle, v); }\n"
	"	void dispatch(int kind) { }\n"
	"}\n";

static const char DESKTOP_CONTAINER[] =
	"class Container extends Component\n"
	"{\n"
	"	void add(Component c) { bzy_desktop_container_add(this.handle, c.handle); }\n"
	"}\n";

static const char DESKTOP_WINDOW[] =
	"class Window extends Container\n"
	"{\n"
	"	void setSize(int w, int h) { bzy_desktop_window_set_size(this.handle, w, h); }\n"
	"	void show() { bzy_desktop_window_show(this.handle); }\n"
	"	void dispose() { bzy_desktop_window_dispose(this.handle); }\n"
	"}\n";

static const char DESKTOP_FRAME[] =
	"class Frame extends Window\n"
	"{\n"
	"	long content;\n"
	"	WindowListener winListener;\n"
	"	Frame()\n"
	"	{\n"
	"		this.handle = bzy_desktop_frame_new();\n"
	"		this.content = bzy_desktop_frame_content(this.handle);\n"
	"		this.regIndex = Desktop.register(this);\n"
	"		bzy_desktop_listen_window_close(this.handle, this.regIndex);\n"
	"	}\n"
	"	void setTitle(string t) { bzy_desktop_frame_set_title(this.handle, t); }\n"
	"	void add(Component c) { bzy_desktop_border_add(this.content, c.handle, 4); }\n"
	"	void addRegion(Component c, int region) { bzy_desktop_border_add(this.content, c.handle, region); }\n"
	"	void addWindowListener(WindowListener l) { this.winListener = l; }\n"
	"	void dispatch(int kind)\n"
	"	{\n"
	"		if (kind == 1)\n"
	"		{\n"
	"			if (this.winListener != null)\n"
	"			{\n"
	"				WindowEvent e = new WindowEvent(this);\n"
	"				this.winListener.windowClosing(e);\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"}\n";

static const char DESKTOP_PANEL[] =
	"class Panel extends Container\n"
	"{\n"
	"	Panel() { this.handle = bzy_desktop_panel_new(); }\n"
	"}\n";

static const char DESKTOP_LABEL[] =
	"class Label extends Component\n"
	"{\n"
	"	Label() { this.handle = bzy_desktop_label_new(); }\n"
	"	void setText(string t) { bzy_desktop_label_set_text(this.handle, t); }\n"
	"}\n";

static const char DESKTOP_BUTTON[] =
	"class Button extends Component\n"
	"{\n"
	"	ActionListener listener;\n"
	"	Button()\n"
	"	{\n"
	"		this.handle = bzy_desktop_button_new();\n"
	"		this.regIndex = Desktop.register(this);\n"
	"	}\n"
	"	void setText(string t) { bzy_desktop_button_set_text(this.handle, t); }\n"
	"	void addActionListener(ActionListener l)\n"
	"	{\n"
	"		this.listener = l;\n"
	"		bzy_desktop_listen_action(this.handle, this.regIndex);\n"
	"	}\n"
	"	void dispatch(int kind)\n"
	"	{\n"
	"		if (this.listener != null)\n"
	"		{\n"
	"			ActionEvent e = new ActionEvent(this);\n"
	"			this.listener.actionPerformed(e);\n"
	"		}\n"
	"	}\n"
	"}\n";

/* Desktop: the entry point, the registration table mapping event reg-indices
   back to widget objects, and the run-loop that pulls events and dispatches to
   listeners in Breezy (C never calls back in). */
static const char DESKTOP_CORE[] =
	"static class Desktop\n"
	"{\n"
	"	static Component[] reg = new Component[4096];\n"
	"	static int regCount = 0;\n"
	"	static bool isEnabled()\n"
	"	{\n"
	"		return bzy_desktop_is_enabled();\n"
	"	}\n"
	"	static int register(Component c)\n"
	"	{\n"
	"		int i = Desktop.regCount;\n"
	"		Desktop.reg[i] = c;\n"
	"		Desktop.regCount = i + 1;\n"
	"		return i;\n"
	"	}\n"
	"	static void run()\n"
	"	{\n"
	"		while (true)\n"
	"		{\n"
	"			int idx = bzy_desktop_next_event();\n"
	"			if (idx < 0) { return; }\n"
	"			int kind = bzy_desktop_event_kind();\n"
	"			Component c = Desktop.reg[idx];\n"
	"			c.dispatch(kind);\n"
	"		}\n"
	"	}\n"
	"}\n";

const char *BZY_DESKTOP_PRELUDE[] = {
	DESKTOP_EXTERNS_A,
	DESKTOP_EXTERNS_B,
	DESKTOP_EXTERNS_C,
	DESKTOP_LISTENERS,
	DESKTOP_EVENTS,
	DESKTOP_BORDERLAYOUT,
	DESKTOP_COMPONENT,
	DESKTOP_CONTAINER,
	DESKTOP_WINDOW,
	DESKTOP_FRAME,
	DESKTOP_PANEL,
	DESKTOP_LABEL,
	DESKTOP_BUTTON,
	DESKTOP_CORE
};
const int BZY_DESKTOP_PRELUDE_COUNT = 14;
