#!/usr/bin/env python3

try:
    import gdb

    class FrameCommitInspector(gdb.Breakpoint):
        def __init__(self):
            super().__init__("commit_buffer")

        def stop(self):
            w = gdb.parse_and_eval("w")
            idx = int(gdb.parse_and_eval("idx"))
            px_ptr = w['buffers'][idx]['pixels']
            if int(px_ptr) == 0:
                print(f"[commit_buffer] idx={idx} pixels pointer is NULL")
                return False

            p0 = int(px_ptr[0])
            p1 = int(px_ptr[1])
            p2 = int(px_ptr[2])
            p3 = int(px_ptr[3])

            width = int(w['buffers'][idx]['width'])
            height = int(w['buffers'][idx]['height'])
            bounds = w['frame']['damage']['bounds']
            dmg_w = int(bounds['width'])
            dmg_h = int(bounds['height'])
            dmg_x = int(bounds['x'])
            dmg_y = int(bounds['y'])
            full_dmg = int(gdb.parse_and_eval("full_damage")) if "full_damage" in [s.name for s in gdb.selected_frame().block()] else -1

            mid_idx = (height // 2) * width + (width // 2) if width > 0 and height > 0 else 0
            p_mid = int(px_ptr[mid_idx]) if width > 0 and height > 0 else 0

            print(f"[commit_buffer] idx={idx} ({width}x{height}) full_dmg={full_dmg} dmg=({dmg_x},{dmg_y} {dmg_w}x{dmg_h})")
            print(f"  first_px=[0x{p0:08x}, 0x{p1:08x}, 0x{p2:08x}, 0x{p3:08x}] (A={(p0>>24)&0xff} R={(p0>>16)&0xff} G={(p0>>8)&0xff} B={p0&0xff})")
            print(f"  mid_px(at {width//2},{height//2})=0x{p_mid:08x}")

            if p0 == 0:
                print("  [WARN] Transparent/zero pixel detected at buffer index 0!")
            return False

    FrameCommitInspector()
    print("[gdb_buffer_inspector] Registered commit_buffer inspector breakpoint.")
except ImportError:
    pass
