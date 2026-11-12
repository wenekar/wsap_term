# macOS release updates

## Current support

`make install` updates the local source build.
It reuses a local signing certificate to keep the application identity stable.
See [Build and install](build.md#macos-updates).

The application has no network update client.
The packaging tool can sign packages with `MACOS_SIGN_IDENTITY`.
It does not notarize packages or publish an update feed.
The application bundle uses `org.wsap.wsap-term` as its identifier.
Both version fields use the Meson project version.

## Release updater

Use [Sparkle 2](https://sparkle-project.org/documentation/) for public application updates.
Do not write a custom update downloader.
Keep the updater disabled in local source builds.
A release requires these steps before automatic updates can work:

1. Sign each release with a Developer ID Application identity from the same team.
2. Keep the bundle identifier unchanged.
3. Enable the hardened runtime with only necessary terminal entitlements.
4. Sign nested binaries and frameworks before signing the application bundle.
5. Notarize the release with Apple and staple the ticket.
6. Increase `CFBundleVersion` for each new release.
7. Set `CFBundleShortVersionString` to the release version.
8. Publish the appcast feed and update archives over HTTPS.
9. Embed the Sparkle EdDSA public key in the application bundle.
10. Sign each update archive with the corresponding EdDSA private key.
11. Store signing keys outside the repository and build logs.

This repository does not configure an appcast URL, Developer ID, or update keys.
Do not enable the updater with test keys or an unverified download server.
A local self-signed certificate is not valid for public distribution.
Switching from a local certificate to Developer ID can require new system permissions.

## Terminal session safety

Check for updates without replacing the running executable binary.
Provide an explicit install-and-restart prompt.
Warn the user before closing active terminal sessions.
Allow the user to postpone the update.
Let Sparkle verify and replace the full bundle.
Do not modify files inside a running signed application bundle.

Test updates from a previous release before publication.
Verify behavior during signature errors, interrupted downloads, and cancelled restarts.
Verify folder access permissions after an update and restart.
Test Apple Silicon and Intel packages if you publish both architectures.

## References

- [Apple: Code signing requirements](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements).
- [Apple: Code signing in depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html).
- [Apple: Notarizing macOS software](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
- [Sparkle: Security and reliability](https://sparkle-project.org/documentation/security-and-reliability/).
