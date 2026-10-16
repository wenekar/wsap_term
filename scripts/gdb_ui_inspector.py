#!/usr/bin/env python3
"""UI inspector commands for gdb. See docs/ui-debugging.md.

    gdb -ex "source scripts/gdb_ui_inspector.py" build/bin/wsap_term [core]

ui-cmd LINE
    Runs one inspector protocol line in the stopped process and prints
    the reply, e.g. `ui-cmd tree --depth 1`, `ui-cmd hit 120 12`,
    `ui-cmd lint`, `ui-cmd -p 0 widget /`. Needs a live process; the
    socket server does not have to be running. Input injection and
    frame waits need a running loop and are refused.

ui-tree [EXPR] [--depth N]
    Prints the widget tree under EXPR (a struct ui_widget or pointer),
    default the primary window's root. Reads memory only, so it also
    works on core files.

ui-widget EXPR
    Prints one widget's fields and resolved box style. Core-safe.
"""

try:
    import gdb
except ImportError:
    gdb = None

if gdb:
    import json

    INVALIDATE = ((1, "measure"), (2, "layout"), (4, "paint"))
    UNITS = {0: "auto", 1: "px", 2: "%", 3: "em", 4: "rem", 5: "ch", 6: "none"}

    def c_string(value):
        try:
            return value.string() if int(value) else None
        except gdb.MemoryError:
            return "<unreadable>"

    def widget_ptr(expr):
        value = gdb.parse_and_eval(expr)
        if value.type.strip_typedefs().code != gdb.TYPE_CODE_PTR:
            value = value.address
        return value.cast(gdb.lookup_type("struct ui_widget").pointer())

    def window_ui(win):
        ui = win["ui"]
        if not int(ui["mounted"]):
            raise gdb.GdbError("window has no UI mount")
        return ui

    def default_root():
        reg = gdb.parse_and_eval("window_ui_host_last")
        if not int(reg):
            raise gdb.GdbError("no window registry; pass a widget expression")
        win = reg["primary"]
        if not int(win) and int(reg["count"]) > 0:
            win = reg["windows"][0]
        if not int(win):
            raise gdb.GdbError("no windows")
        return window_ui(win)["root"].address

    def type_name(w):
        ops = w["ops"]
        name = c_string(ops["name"]) if int(ops) else None
        return name or "widget"

    def rect(r):
        return f"{int(r['x'])},{int(r['y'])} {int(r['width'])}x{int(r['height'])}"

    def children(w):
        c = w["first_child"]
        while int(c):
            yield c
            c = c["next_sibling"]

    def describe(w):
        parts = [type_name(w), rect(w["bounds"])]
        impl = w["impl"]
        cls = c_string(impl["style_class"])
        if cls:
            parts.append("." + cls.replace(" ", "."))
        dirty = [n for bit, n in INVALIDATE if int(impl["invalidation"]) & bit]
        if dirty:
            parts.append("dirty=" + ",".join(dirty))
        if bool(impl["destroyed"]):
            parts.append("DESTROYED")
        if bool(impl["hidden"]):
            parts.append("HIDDEN")
        addr = int(w.address)
        surface = impl["surface"]
        if int(surface):
            state = surface["impl"]
            if int(state["focused"]) == addr:
                parts.append("FOCUSED")
            if int(state["captured"]) == addr:
                parts.append("CAPTURED")
        parts.append(f"({addr:#x})")
        return "  ".join(parts)

    def walk(w, path, depth, max_depth):
        print("  " * depth + f"{path:<10} " + describe(w))
        if max_depth is not None and depth >= max_depth:
            n = int(w["impl"]["child_count"])
            if n:
                print("  " * (depth + 1) + f"... {n} children")
            return
        for i, c in enumerate(children(w)):
            walk(c.dereference(), path.rstrip("/") + f"/{i}", depth + 1,
                 max_depth)

    class UiCmd(gdb.Command):
        """ui-cmd LINE -- run an inspector protocol line in-process."""

        def __init__(self):
            super().__init__("ui-cmd", gdb.COMMAND_DATA)

        def invoke(self, arg, from_tty):
            quoted = json.dumps(arg)
            ptr = gdb.parse_and_eval(f"(char *)window_debug_eval({quoted})")
            if not int(ptr):
                raise gdb.GdbError("window_debug_eval returned NULL")
            text = ptr.string()
            gdb.parse_and_eval(f"(void)free((void *){int(ptr)})")
            reply = json.loads(text)
            if not reply.get("ok"):
                raise gdb.GdbError(reply.get("error", "error"))
            print(json.dumps(reply.get("result"), indent=2))

    class UiTree(gdb.Command):
        """ui-tree [EXPR] [--depth N] -- print the widget tree (core-safe)."""

        def __init__(self):
            super().__init__("ui-tree", gdb.COMMAND_DATA)

        def invoke(self, arg, from_tty):
            args = gdb.string_to_argv(arg)
            max_depth = None
            if "--depth" in args:
                i = args.index("--depth")
                max_depth = int(args[i + 1])
                del args[i:i + 2]
            root = widget_ptr(" ".join(args)) if args else default_root()
            walk(root.dereference(), "/", 0, max_depth)

    class UiWidget(gdb.Command):
        """ui-widget EXPR -- print one widget (core-safe)."""

        def __init__(self):
            super().__init__("ui-widget", gdb.COMMAND_DATA)

        def invoke(self, arg, from_tty):
            w = widget_ptr(arg).dereference()
            print(describe(w))
            print(f"  measured  {int(w['measured_size']['x'])}x{int(w['measured_size']['y'])}")
            print(f"  parent    {int(w['parent']):#x}  children {int(w['impl']['child_count'])}")
            for key in ("style_id", "style_tag"):
                s = c_string(w['impl'][key])
                if s:
                    print(f"  {key:<9} {s}")
            style = w["style"]
            for box in ("padding", "margin"):
                edges = []
                for i in range(4):
                    v = style[box]["v"][i]
                    unit = int(v["unit"])
                    edges.append("auto" if unit == 0 else
                                 f"{float(v['val']):g}{UNITS.get(unit, '?')}")
                print(f"  {box:<9} {' '.join(edges)}")
            print(f"  scale     dpi={float(w['style_scale']['dpi_scale']):g} "
                  f"cell={float(w['style_scale']['cell_w']):g}x"
                  f"{float(w['style_scale']['cell_h']):g}")

    UiCmd()
    UiTree()
    UiWidget()
