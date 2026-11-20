# Terminal grid renderer

This directory turns a terminal snapshot into the generic grid view of
`src/render/grid.h`, and draws it through `wsap-render` (the render
library).

The app calls `render_draw()`, `render_cursor_cell()`, and
`render_gpu_draw()` here. Each call builds a `struct render_grid_view` and
calls the render library once. The renderer keeps one frame and one draw
call, as before.

The terminal content owns this library. It sits above `wsap-render` and
below `wsap-terminal` in the include graph. `wsap-render` has no terminal
core dependency.

## Modules

| Path | Job |
|---|---|
| `term_frame.h` | Public terminal frame: the snapshot values of one frame. The old `render/term_frame.h` path forwards here. |
| `term_frame.c` | Adapts a terminal snapshot to the terminal frame. |
| `walk.c/.h` | Applies the VT cell policy, segments text, shapes glyphs, and resolves each cell to a `render_cell`. |
| `view.c/.h` | Builds the grid view, the row key, the line sizes, and the walk ops. |
| `draw.c` | The public `render_draw()`, `render_cursor_cell()`, and `render_gpu_draw()` calls. |

## Rules

- The view points at the terminal frame and its snapshot only during the
  call. The renderer keeps no pointer after it returns.
- The row key mix stays the same as the old `render_surface_row_key()`, so a
  frame keeps the same row cache behavior.
- The walk and the raster agree on the line size, the cursor, the selection,
  and the decoration kinds.
