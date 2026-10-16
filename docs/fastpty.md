# Fastpty implementation notes

## Unicode environment

Child shells receive `TERM=wsap_term-256color`, `TERM_PROGRAM=wsap_term`, and
`UNICODE_VERSION=18.0.0`, with the Unicode version taken from the terminal's
property-table version. `COLORTERM=truecolor` is exported for direct truecolor
detection. An inherited `TERM_PROGRAM_VERSION` is cleared because it describes
the parent terminal, not wsap_term.

The Unicode version describes the terminal's Unicode tables; glyph coverage
also depends on installed fonts. Applications must opt in to reading this
environment variable. For example, older Python `wcwidth` versions use it,
while newer versions only ship their latest tables.

`terminal-quick-test` uses `Terminal::Capabilities` environment heuristics.
Its detector defaults unknown UTF-8 terminals to `Uni1` and does not read
`UNICODE_VERSION`. That label is not a measured Unicode conformance result;
the detector needs a wsap_term entry to report `Uni13`.

## Output post-processing

The fast pipe carries the bytes that the child wrote.
The reader passes these bytes to the parser without a copy or scan.
The pipe bypasses the PTY output processing.
The session reads `OPOST` and `ONLCR` from the PTY settings.
If both flags are set, it feeds the bytes with `TERM_FEED_ONLCR`.
This feed flag makes LF return the cursor to column zero.

Direct PTY output keeps the kernel output processing.
The kernel applies the child's settings when the child writes.
The parser does not add a carriage return to these bytes.
A later change to the shell's input mode cannot change their line endings.

The feed flag is separate from DEC LNM mode (ANSI mode 20).
LNM belongs to the child.
A terminal reset clears LNM but does not change the transport policy.
A query for mode 20 reports only the child's LNM setting.

A program that clears `ICANON` gets its output moved to the pty; see
*Raw-mode routing* below. The kernel then applies that program's own OPOST
setting, so a raw-mode program that clears OPOST gets a bare LF.

Set `WSAP_FAST_PIPE=0` to disable the fast pipe and fall back to ordinary pty
output on all platforms; on opt-in platforms `WSAP_FAST_PIPE=1` remains
required. That is also the escape hatch for an application that needs a single
ordered output stream, or true raw-mode line endings.

## Privilege-command fallback

On Linux, fastpty switches fast-pipe stdout/stderr back to the real PTY when
its `execve`, `posix_spawn`, or `posix_spawnp` hook launches commands that require
direct PTY access (such as `sudo`, `doas`, `su`, `pkexec`, `bwrap`, or `agy`).
The list is `PTY_TTY_CMDS` in `src/pty/cmdline.h`.
Explicit file-open redirects are applied by the child after the fallback's
temporary descriptor substitution. Stream-duplication actions that reference
stdout or stderr (for example, `2>&1`) therefore do not retain the original
fast-pipe descriptors. Calls without such actions switch fast-pipe
stdout/stderr to the real PTY. If execution fails, the original output
descriptors are restored; ordinary commands continue to use the fast pipe.

Fast-pipe sessions set `fish_use_posix_spawn=0` for compatibility with older
Fish versions. Direct system calls and launch mechanisms outside these libc
hooks bypass the fallback. This fallback is not implemented for the optional
macOS fast-pipe backend.

## Terminal lookup

The child gets no extra file descriptors: only stdio is open at exec.
When a shimmed call must reach the terminal, the shim opens the slave by
the path in `WSAP_PTY_PATH`. If that fails, it tries `/dev/tty`, then
fd 0. The shim remembers the device and inode of the fd it opened. If the
app closes that fd number and reuses it, the shim sees a different file.
It then opens the terminal again and does not close the app's fd.

## Spawn and hangup

`pty_spawn` splits the command line into words before it forks. Blanks
separate words. Single quotes are literal. Double quotes allow `\"`, `\\`,
`\$` and `` \` `` escapes. A backslash outside quotes escapes the next
character. There is no expansion. The parent also builds the environment
and resolves `PATH`. After `fork()` the child only calls async-signal-safe
functions, so a lock held by another thread cannot deadlock it.

`pty_destroy` sends SIGHUP and SIGCONT to the child and its process
group, closes the descriptors, and returns at once. A background thread
reaps the child. If the child is still alive after 2 seconds, the thread
sends SIGKILL. This gives a shell time to save history on hangup.

## Raw-mode routing

On Linux, the shim watches `tcsetattr` and the `TCSETS*` ioctls. When a
program clears `ICANON` on the session's terminal, the shim moves that
program's fast-pipe stdout and stderr to the pty. When `ICANON` returns, it
moves them back. Shells do this at every prompt: line editing runs on the
pty, and the commands they start run on the fast pipe. Interactive programs
do not need the pipe's throughput. Bulk output (`cat`, compilers, logs)
stays in cooked mode and keeps it.

Each move first passes a barrier (`src/pty/sync.h`). The shim connects to
the session's sync socket, whose abstract name is in `WSAP_SYNC`. It names
the transport it leaves, and waits up to 200 ms for an acknowledgement. The
ring worker drains that transport into the ring before it replies. Output
the program wrote before the move is therefore ahead of output written
after it. If the barrier fails or times out, the output stays where it is:
a lost move costs speed or line endings, never order. The barrier can make
`tcsetattr` wait while the terminal clears a backlog.

Routing is per process. `stty raw` changes the terminal but routes only
`stty` itself. Set `WSAP_TTY_ROUTE=0` to turn routing off. Darwin has no
barrier yet, so it does not route.

## Reader ownership and stream ordering

The ring worker is the only reader. It reads the fast pipe and the PTY
master into one ring, so the ring order is the order it read them in.
Switch points between the two are recorded beside the ring, and each
`data` callback names the transport its bytes came from. The session uses
that to pick the newline policy for each span. If the worker cannot start,
the main loop reads both descriptors.

Output from independent processes has no defined order across the two
transports, just as it has none on a single pty. Within one process, the
barrier keeps the order across a routing move. A program that writes to
`/dev/tty` directly while its stdout is on the fast pipe gets no barrier,
so those two streams can still interleave. `WSAP_FAST_PIPE=0` places all
output on the PTY.

## Writers and input backpressure

The shim recognizes duplicated fast-pipe descriptors by their file identity, so
configuring a nested PTY does not disturb the outer terminal. Exec propagation
covers stderr-only fast-pipe output and avoids adding preload entries already
present in the child environment.

Keyboard bytes, terminal replies, and paste data share the input queue. Raw
input retains its control bytes, and pending writes resume on `POLLOUT`.
This prevents short writes from dropping input and prevents subsequent keys
or replies from being inserted inside a pending paste.

The privilege-command spawn fallback temporarily changes process-wide
stdout/stderr descriptors, so a concurrent writer in another thread can observe
the substitution. Resolving that needs launch routing, not reader changes.

## Regression checks

- `meson test -C build pty-fast-test`: byte fidelity of a spawned child's
  output (high-entropy payload including raw LF, which also detects a silent
  fallback to the pty) and locale environment setup.
- `meson test -C build term-test`: that the transport's newline policy survives
  RIS, DECSTR and `ESC[20l`, and that the DEC LNM mode on its own still does
  not.
- `meson test -C build fastpty-test`: actual preload/exec routing, duplicated descriptors,
  unrelated PTYs, stderr-only propagation, and preload deduplication.
- `meson test -C build pty-route-test`: a raw-mode helper switches mode 200
  times behind 32 KiB pipe bursts, with a slow consumer that keeps the ring
  full. It checks that raw-mode output arrives on the pty, cooked output on
  the pipe, and the stream stays in program order.
- `meson test -C build paste-test`: partial writes and byte-preserving input after queued paste.
- `meson test -C build pty-order-test`: optional Linux integration test requiring Sway and `-Dintegration_tests=true`; starts
  a private headless compositor and verifies 8,000 ordered PTY cursor replies
  while fastpipe carries concurrent write traffic. A test-only preload library
  delays background PTY reads after consumption to exercise competing-reader
  schedules.
