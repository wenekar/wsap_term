# Compatibility

The current version is 0.1.0.
This version does not give a 1.0 compatibility guarantee.

Public headers expose struct layouts, enums, constants, and inline functions.
A changed struct layout breaks binary compatibility.
Call `wsap_vtcore_version()` to check the library version.
`WSAP_VTCORE_VERSION` generates this numeric version value.

The Meson dependency supplies headers and the library from one build.
The amalgamation generator emits a matching C source file and header.
Keep these files from the same source revision.
Do not mix headers from one release with a library from a different release.

The core implements standard terminal protocols.
It does not guarantee complete compatibility with every terminal emulator.
Internal parse and render structures do not form a stable ABI.
Use the public snapshot interface to read screen data.
