# Terminal grid renderer

The terminal grid renderer adapts a terminal snapshot to the [framework grid interface](../../../subprojects/wsap-framework/src/render/grid.h). The app uses `render_draw()` for frames and `render_cursor_cell()` for the cursor overlay. Renderer tests can use `render_gpu_draw()` to exercise the GPU backend directly.

The terminal content owns this library. It sits above `wsap-render` and
below `wsap-terminal` in the include graph. `wsap-render` has no terminal
core dependency.

## Modules

| Path | Job |
|---|---|
| `term_frame.h` | Public terminal frame: the snapshot values of one frame |
| `term_frame.c` | Adapts a terminal snapshot to the terminal frame. |
| `walk.c/.h` | Applies the VT cell policy, segments text, shapes glyphs, and resolves each cell to a `render_cell`. |
| `view.c/.h` | Builds the grid view, the row key, the line sizes, and the walk ops. |
| `draw.c` | The public `render_draw()`, `render_cursor_cell()`, and `render_gpu_draw()` calls. |

## Rules

- The view points at the terminal frame and its snapshot only during the
  call. The renderer keeps no pointer after it returns.
- The row key includes the source row key and the selection and cursor state that affect its pixels. A zero source key prevents row reuse.
- The walk and the raster agree on the line size, the cursor, the selection,
  and the decoration kinds.
