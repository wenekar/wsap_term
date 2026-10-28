# Compatibility

The current version is 0.1.0. It is not a 1.0 compatibility promise.

The public headers expose several struct layouts, enums, constants, and inline
helpers. Changing a public struct layout can break binary consumers. Check
`wsap_vtcore_version()` when a program needs to reject a different library
version. The version is a numeric value made with `WSAP_VTCORE_VERSION`.

The Meson dependency supplies headers and the library from one build. The
amalgamation generator emits a matching C file and header. Keep those files
from the same source revision. Do not mix headers from one release with a
library from another release unless the application has tested that pair.

The core follows terminal protocols, but it does not promise complete
compatibility with every terminal emulator. Its parsing and rendering data
are not an ABI for screen internals; use the public snapshot interface.
