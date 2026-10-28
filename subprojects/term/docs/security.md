# Security boundary

Treat terminal output as untrusted input. A remote program can send control
sequences, text, links, clipboard data, titles, and notification requests.
The core parses these bytes. It does not grant the program host permissions.

## Host policy

- Leave unused callbacks unset.
- Ask for permission before applying a clipboard request.
- Treat clipboard bytes as a byte span. They can contain NUL bytes.
- Check link schemes before opening a link. Do not pass links to a shell.
- Treat titles, paths, and notifications as untrusted text.
- Copy callback data before returning if the host needs it later.
- Queue terminal changes until the enclosing callback call has returned.
- Use a bounded queue for replies and host events. Do not block the writer on it.
- Limit the rate of host side effects. A payload limit is not a rate limit.

The core does not open URLs, start processes, or access the system clipboard.
The host must enforce those boundaries.

## Resource limits

The current implementation limits a control-string buffer to 16 MiB.
It limits one decoded OSC 52 clipboard request to 100 KiB.
It limits a hyperlink URI to 4096 bytes and the link table to 8192 entries.
It retains at most 20 code points in one grapheme cluster.
These are implementation limits, not a promise to accept every sequence up to
that size. Oversized or unsupported input can be discarded.

The public width limit is `WSAP_VTCORE_MAX_COLS`. Scrollback has a public line
limit and a separate internal byte budget. A line limit does not bound total
process memory. Screen size, snapshots, graphemes, and host queues also use
memory. Set dimensions from a trusted host policy. Keep snapshot counts small.
Use process resource limits when strict memory or CPU isolation is required.

A successful feed accepts the input span. It does not report every internal
allocation failure. Do not describe this interface as a lossless recorder
under memory exhaustion. See [api.md](api.md) for failure rules.

## Reporting

Use the repository's private vulnerability report feature if it is enabled.
If it is not enabled, ask the maintainer for a private contact without posting
exploit details. Include the source revision, compiler, platform, smallest
input, and sanitizer report. Remove secrets from recorded terminal sessions.

No external security audit or continuous fuzzing service is claimed here.
CI runs the fuzz target for ten minutes on each change, and
[testing.md](testing.md) describes the other checks. These checks help find
faults. They do not prove that the core has no security defects.
