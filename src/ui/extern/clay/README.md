# Clay source

Source: https://github.com/nicbarker/clay
Commit: e6cc36941ab2af5d81107617039d6f527a1c660b

The header has a small local extension.
`Clay_LayoutConfig.intrinsicSize` accepts a widget's natural content size.
Fit size includes this content.
Minimum size still comes from the sizing limits.
A fixed zero size stays zero.
These changes have `WSAP` marks in the header.
The license is in `LICENSE.md`.
`src/ui/layout.c` compiles the implementation.
The build does not need a network connection.

To update Clay, replace the header and license from one commit.
Apply the local extension again.
Update the commit ID above.
Run the UI tests and the UI benchmark.
Check the settings screen with the UI inspector.
