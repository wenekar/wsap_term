# Fastpty implementation notes

## Unicode environment

Child shells receive `TERM=wsap_term-256color` and `TERM_PROGRAM=wsap_term`.
They also receive `UNICODE_VERSION=18.0.0`.
The property-table version of the terminal sets `UNICODE_VERSION`.
The terminal exports `COLORTERM=truecolor` for direct truecolor detection.
The terminal clears an inherited `TERM_PROGRAM_VERSION`.
That variable describes the parent terminal, not wsap_term.

The Unicode version describes the terminal's Unicode tables.
Glyph coverage also depends on installed fonts.
Applications must opt in to read this environment variable.
For example, older versions of Python `wcwidth` read it.
Newer versions include only their latest built-in tables.

`terminal-quick-test` uses `Terminal::Capabilities` environment heuristics.
Its detector assigns unknown UTF-8 terminals to `Uni1`.
It does not read `UNICODE_VERSION`.
The `Uni1` label is not a measured Unicode conformance result.
The detector requires a wsap_term entry to report `Uni13`.

## Output post-processing

The fast pipe carries the bytes that the child wrote.
The reader passes these bytes to the parser without a copy or scan.
The pipe bypasses PTY output processing.
The session reads `OPOST` and `ONLCR` from the PTY settings.
If both flags are set, the session feeds the bytes with `TERM_FEED_ONLCR`.
This feed flag makes LF return the cursor to column zero.

Direct PTY output keeps kernel output processing.
The kernel applies child settings when the child writes.
The parser does not add a carriage return to these bytes.
A later change to the shell input mode cannot change their line endings.

The feed flag is separate from DEC LNM mode (ANSI mode 20).
LNM belongs to the child.
A terminal reset clears LNM, but does not change transport policy.
A query for mode 20 reports only the child LNM setting.

When a program clears `ICANON`, the shim routes its output to the PTY.
See *Raw-mode routing* below.
The kernel applies the program's own `OPOST` setting.
Therefore, a raw-mode program that clears `OPOST` receives a bare LF.

Set `WSAP_FAST_PIPE=0` to disable the fast pipe.
The session then falls back to standard PTY output on all platforms.
On opt-in platforms, `WSAP_FAST_PIPE=1` remains required to enable the fast pipe.
Use `WSAP_FAST_PIPE=0` if an application requires a single ordered output stream or true raw-mode line endings.

## Privilege-command fallback

On Linux, fastpty hooks `execve`, `posix_spawn`, and `posix_spawnp`.
Some commands require direct PTY access.
Examples include `sudo`, `doas`, `su`, `pkexec`, `bwrap`, and `agy`.
`PTY_TTY_CMDS` in `src/pty/cmdline.h` defines this command list.
When a hook launches one of these commands, fastpty redirects fast-pipe stdout and stderr to the real PTY.

The child process applies file-open redirections after this descriptor change.
Stream duplications for stdout or stderr (such as `2>&1`) do not keep fast-pipe descriptors.
They duplicate the real PTY descriptors instead.
Calls without stream duplications switch stdout and stderr to the real PTY.
If execution fails, fastpty restores the original output descriptors.
Ordinary commands continue to use the fast pipe.

Fast-pipe sessions set `fish_use_posix_spawn=0` for compatibility with older Fish versions.
Direct system calls and launch mechanisms outside these libc hooks bypass the fallback.
The optional macOS fast-pipe backend does not implement this fallback.

## Terminal lookup

The child receives no extra file descriptors.
Only standard streams remain open at exec.
When a shim call must access the terminal, the shim opens the slave path from `WSAP_PTY_PATH`.
If that open fails, the shim tries `/dev/tty`, then file descriptor 0.
The shim records the device number and inode of the opened descriptor.
If the application closes that descriptor and reuses the number, the shim detects the file change.
The shim opens the terminal again.
The shim does not close the application descriptor.

## Spawn and hangup

`pty_spawn` splits the command line into words before it forks.
Spaces and tabs separate words.
Single quotes keep characters literal.
Double quotes allow `\"`, `\\`, `\$`, and `` \` `` escapes.
A backslash outside quotes escapes the next character.
The parser performs no variable or shell expansion.
The parent process builds the environment and resolves `PATH`.
After `fork()`, the child calls only async-signal-safe functions.
A lock held by another thread cannot cause a deadlock.

`pty_destroy` sends SIGHUP and SIGCONT to the child and its process group.
It closes all descriptors and returns immediately.
A background thread reaps the child process.
If the child remains alive after 2 seconds, the thread sends SIGKILL.
This delay gives a shell time to save history on hangup.

## Raw-mode routing

On Linux, the shim monitors `tcsetattr` and the `TCSETS*` ioctls.
A program can clear `ICANON` on the session terminal.
When this occurs, the shim moves that program's stdout and stderr to the PTY.
When `ICANON` is restored, the shim moves them back to the fast pipe.
Shells perform this cycle at each prompt.
Line editing runs on the PTY.
Commands started by the shell run on the fast pipe.
Interactive programs do not need fast-pipe throughput.
Bulk output (`cat`, compilers, logs) runs in cooked mode and uses the fast pipe.

Each route change passes a synchronization barrier (`src/pty/sync.h`).
The shim connects to the session sync socket.
`WSAP_SYNC` contains the abstract name of this socket.
The shim identifies the transport that it leaves.
It waits up to 200 ms for an acknowledgment.
The ring worker parses data from that transport into the terminal.
Then the ring worker sends the acknowledgment.
Thus, output written before the move processes before output written after the move.
If the barrier fails or times out, output remains on the current transport.
A failed move can reduce throughput or alter line endings.
A failed move never changes output order.
The barrier can delay `tcsetattr` while the terminal clears backlog data.

Routing applies per process.
`stty raw` modifies terminal settings, but routing affects only `stty` itself.
Set `WSAP_TTY_ROUTE=0` to disable routing.
macOS (Darwin) has no barrier support, so macOS does not route.

## Reader ownership and stream ordering

The ring worker is the only reader.
It reads from the fast pipe and the PTY master.
It parses bytes into the terminal immediately after each read.
The terminal receives bytes in the exact order of those reads.
Each `data` callback identifies the source transport.
The session uses this source to select the newline policy for each data span.
The worker starts before the session returns.
If the worker cannot start, the session does not open.

The worker wakes the loop thread once for each 32 KiB of parsed data.
It also wakes the loop thread when an input source runs dry.
Short output wakes the loop thread immediately.
A high-volume stream does not signal the loop thread for each small read.
`benchmarks/pty-wake-batching.md` records benchmark measurements.

Output from independent processes has no guaranteed order across the two transports.
Standard PTY operation has this same limitation.
Within a single process, the barrier preserves output order across route changes.
A program can write directly to `/dev/tty` while its stdout uses the fast pipe.
That write uses no barrier, so the two streams can interleave.
Set `WSAP_FAST_PIPE=0` to route all output through the PTY.

## Writers and input backpressure

The shim recognizes duplicated fast-pipe descriptors by their file identity.
Therefore, configuring a nested PTY does not affect the outer terminal.
Exec propagation handles stderr-only fast-pipe output.
It also skips preload entries that already exist in the child environment.

Keyboard bytes, terminal replies, and paste data share one input queue.
Raw input keeps its control bytes.
Pending writes resume when the transport reports `POLLOUT`.
This rule prevents partial writes from dropping input.
It also prevents subsequent keys or replies from breaking into a pending paste.
The input queue has a mutex lock.
A worker thread can push a terminal reply while the loop thread pushes a key event.

The privilege-command spawn fallback temporarily changes process-wide stdout and stderr descriptors.
A concurrent writer thread can observe this substitution.
Fixing this issue requires launch routing instead of reader changes.

## Regression checks

- `meson test -C build pty-fast-test`: checks byte fidelity of child output and locale environment setup. It tests high-entropy data with raw LF bytes to detect silent PTY fallbacks.
- `meson test -C build term-test`: verifies that the transport newline policy survives RIS, DECSTR, and `ESC[20l`, and that DEC LNM mode alone does not alter transport policy.
- `meson test -C build fastpty-test`: tests preload routing, exec propagation, duplicated descriptors, unrelated PTY isolation, stderr-only propagation, and preload deduplication.
- `meson test -C build pty-route-test`: switches raw and cooked mode 200 times behind 32 KiB pipe bursts with a slow consumer. It verifies that raw output uses the PTY, cooked output uses the pipe, and data order remains correct.
- `meson test -C build paste-test`: tests partial writes and byte-preserving input after queued paste data.
- `meson test -C build pty-order-test`: optional Linux integration test that requires Sway and `-Dintegration_tests=true`. It starts a headless compositor and checks 8,000 ordered PTY cursor replies during concurrent fast-pipe write traffic. A test preload library delays background PTY reads after consumption to test competing-reader schedules.
