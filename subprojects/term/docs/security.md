# Security boundary

Treat terminal output as untrusted input.
Remote programs can transmit control sequences, text, hyperlinks, clipboard payloads, window titles, and notification requests.
The core parses these input bytes.
The core does not grant host permissions to remote programs.
The core never opens URLs, spawns processes, or accesses the system clipboard directly.
The host application must enforce all security boundaries.

## Host policy rules

Follow these security rules in the host application:

- Leave unused callbacks set to `NULL`.
- Prompt users for permission before applying clipboard write requests.
- Treat clipboard payloads as raw byte spans that can contain `NUL` bytes.
- Validate URI schemes before opening links. Never pass unvalidated links to a system shell.
- Treat window titles, directory paths, and notifications as untrusted strings.
- Copy callback payload data before returning if the host needs it later.
- Defer internal terminal mutations until the active callback returns.
- Use a bounded queue for terminal replies and host events. Never block the input writer on this queue.
- Rate-limit host side effects. Payload size limits do not prevent rapid event flooding.

## Resource limits

The parser enforces these implementation resource limits:

| Resource | Maximum limit | Behavior on overflow |
| --- | --- | --- |
| Control string buffer | 16 MiB | Drops excess bytes. |
| Decoded OSC 52 clipboard payload | 100 KiB | Discards oversized payload. |
| Hyperlink URI length | 4096 bytes | Discards oversized URI. |
| Hyperlink table capacity | 8192 entries | Replaces oldest entries. |
| Window title length | 4096 bytes | Truncates title to limit. |
| Saved window title stack | 10 titles | Discards oldest saved title. |
| Working directory path length | 4096 bytes | Discards oversized path. |
| User variable name length | 255 bytes | Discards oversized variable. |
| User variable value length | 4096 bytes | Discards oversized value. |
| History mark table capacity | 65536 entries | Drops oldest history marks. |
| Grapheme cluster size | 20 code points | Drops excess code points. |
| Terminal column count | `WSAP_VTCORE_MAX_COLS` | Clamps columns to maximum. |

These values represent internal implementation limits.
They do not guarantee that the parser accepts every sequence up to that size.
The parser discards malformed or unsupported input.

## Memory boundaries

Scrollback configuration uses a public line count limit and an internal byte budget.
Line count limits alone do not constrain total process memory.
Screen dimensions, snapshots, grapheme allocations, and event queues also consume memory.
Apply window dimensions from trusted host configuration.
Keep active snapshot counts low.
Use operating system resource limits (such as `ulimit` or cgroups) for strict memory and CPU isolation.

A successful write feed accepts the input byte span.
It does not guarantee successful allocation for every internal element.
Do not treat this interface as a lossless log during memory exhaustion.
See [api.md](api.md) for error handling rules.

## Vulnerability reporting

Report vulnerabilities through the repository private reporting feature if available.
If private reporting is unavailable, contact the project maintainer directly before publishing exploit details.
Include these details in reports:

- Git commit hash
- Compiler and version
- Target platform and architecture
- Minimal reproducer input
- Sanitizer crash logs

Remove sensitive credentials and private data from recorded terminal sessions.

This project does not claim continuous third-party security audits.
CI executes libFuzzer targets for ten minutes on each commit.
See [testing.md](testing.md) for testing details.
These tests identify defects but do not guarantee complete defect absence.
