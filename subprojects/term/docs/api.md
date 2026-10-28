# API guide

Include `wsap_vtcore/wsap_vtcore.h`. Use C11. The public header also works
as C++ and declares functions with C linkage.

## Create and free

Zero-initialize `struct wsap_vtcore_options`. Passing `NULL` has the same
effect as zero options. A zero width or height selects 80 columns or 24 rows.
Zero scrollback selects `WSAP_VTCORE_SCROLLBACK_DEFAULT`; use
`WSAP_VTCORE_SCROLLBACK_MAX` as the maximum. Zero theme selects built-in
colors. A null or empty name selects `wsap_vtcore`. Zero cursor style selects
a block cursor. A zero reserved field is required.

`wsap_vtcore_new` returns `NULL` for invalid options or allocation failure.
It copies option values. It does not copy callback userdata. The host owns
that data and must keep it valid until the core is freed. Free a core with
`wsap_vtcore_free`; do not call any other core function after free.

Allocation failure can also affect later operations. Check boolean results
where provided. A function without a failure result does not report all
internal allocation failures. An allocation failure can lose output, but the
terminal stays valid. When memory returns, a full reset (`ESC c`) gives the
same screen as a new terminal. `alloc-failure-test` checks this for each
allocation in its workloads.

## Threads and callbacks

Core operations on one terminal serialize through an internal writer lock.
`wsap_vtcore_lock` and `wsap_vtcore_unlock` let a host group calls under one
lock. Lock and unlock on the same thread, and balance every lock. Calls made
inside a held lock are re-entrant on that thread. Do not free a terminal while
another thread uses it. Snapshot objects are separate caller-owned objects.
Multiple threads can read one snapshot if no thread updates or frees it.

Callbacks run synchronously during core operations. Callback text is borrowed
and must not be retained after the callback returns. Callback userdata remains
owned by the host. Callback work runs while the terminal is locked. Keep it
short. Avoid waiting for a thread that may need the same terminal. A reply
callback receives the response bytes and their length; it must send or copy
them before it returns.

Do not feed, resize, free, or otherwise change the terminal from a callback.
The recursive lock does not make parser re-entry safe. Queue such work until
the enclosing call returns. Explicit locking outside callbacks can group
ordinary API calls.

## Feeding output

Pass a byte span to `wsap_vtcore_feed`. `NULL` is valid only when `len` is
zero. The only defined feed flag is `WSAP_VTCORE_FEED_ONLCR`; other flag bits
are rejected. The function returns false for a null terminal, invalid byte
pointer, or unsupported flags. These argument failures happen before parsing
input. True means the supplied span was accepted by the parser. It does not
mean that every sequence was supported or that every internal allocation
succeeded. The caller can release or reuse the input buffer after return.

Feeding arbitrary chunks is supported, including chunks that end inside a
multibyte character or escape sequence. Preserve byte order between calls.

## Snapshots and text

Create a snapshot with `wsap_vtcore_snapshot_new`; free it with
`wsap_vtcore_snapshot_free`. `wsap_vtcore_snapshot_update` copies the current
view and returns false for invalid pointers or reported allocation failure.
The view, row, palette, and selection data belong to the snapshot. Getter
pointers remain valid until the next update attempt or free. A failed update
preserves the prior values, but it can move the backing buffers. Re-fetch all
getter pointers after any update attempt.

The snapshot is a copy. It remains valid after its source terminal is freed.
Read it without holding the terminal lock. Multiple readers can share a
snapshot, but updates and free need exclusive access. One snapshot can capture
different terminals in turn.

A cell with `text & WSAP_VTCORE_TEXT_CLUSTER` contains a snapshot cluster
handle, not a Unicode scalar value. Call `wsap_vtcore_snapshot_cluster` to
obtain its code points. The return value is the full cluster length, even
when the supplied capacity is smaller. A plain cell returns one code point.
A zero return means the snapshot or cluster data is invalid. Use text values
from the current snapshot. Do not retain cluster handles across updates.
A width-zero
cell is a continuation cell; do not print its text again. For width-one and
width-two cells, render the scalar or all cluster code points once. Cell width
is terminal cell width, not a pixel size.

The view and its palette pointer are snapshot-owned. Resolve palette slot
colors through `wsap_vtcore_view_color` while that view remains valid.

## Input encoding and allocated strings

Input encoders write into caller-provided buffers and return a result with a
status and length. Check `status`; do not assume the buffer is large enough.
`WSAP_VTCORE_INPUT_BUFFER_FULL` means the result did not fit. The encoder can
write a partial prefix on this path. Send no bytes unless the status is
`WSAP_VTCORE_INPUT_BYTES`. The encoder does not allocate its output.

Functions that return `char *`, including selection, link, cwd, pointer shape,
and history text getters, return allocated text or `NULL`. Release returned
text with `wsap_vtcore_text_free`. Do not use `free` as the public ownership
rule.

## Invalid arguments

Null handling is function-specific. Several query functions return zero or
null values for null input, while mutating calls may do nothing. Read each
function's return contract before use. Do not pass invalid enum values or
out-of-range coordinates unless the function documents its response.
