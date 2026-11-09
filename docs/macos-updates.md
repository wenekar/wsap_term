# macOS release updates

## Current support

`make install` updates the local source build.
It reuses a local signing certificate to keep the app identity stable.
See [build and install](build.md#macos-updates).

The app has no network update client.
The package tool can sign with `MACOS_SIGN_IDENTITY`.
It does not notarize packages or publish an update feed.
The bundle uses `org.wsap.wsap-term` as its identifier.
Both version fields currently use the Meson project version.

## Release updater

Use [Sparkle 2](https://sparkle-project.org/documentation/) for public app updates.
Do not add a custom download-and-run updater.
Keep the updater off in local source builds by default.
A release needs these parts before automatic updates can work:

1. Sign each release with a Developer ID Application identity from the same team.
2. Keep the bundle identifier stable.
3. Enable the hardened runtime with only the entitlements that the terminal needs.
4. Sign nested code before the app.
5. Notarize the release with Apple and staple the ticket.
6. Give each release a higher `CFBundleVersion`.
7. Set `CFBundleShortVersionString` to the release version.
8. Publish the appcast and update archives over HTTPS.
9. Put the Sparkle EdDSA public key in the app.
10. Sign each update archive with its EdDSA private key.
11. Keep signing keys outside the repository and public build logs.

The appcast URL, Developer ID, and update keys are not set in this repository.
Do not enable an updater with test keys or an unverified download path.
A local self-signed certificate is not a public release identity.
Changing from a local certificate to Developer ID can require new privacy grants.

## Terminal session safety

Check for updates without replacing the running executable.
Offer an explicit install-and-restart action.
Warn the user before closing active terminal sessions.
Allow the user to defer the update.
Let Sparkle verify and replace the full bundle.
Do not patch files inside the running signed app.

Test updates from a prior release before publication.
Check signature failures, interrupted downloads, and declined restarts.
Check folder grants after both an update and a relaunch.
Check Apple Silicon and Intel packages if both are published.

## References

- [Apple: Code signing requirements](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements).
- [Apple: Code signing in depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html).
- [Apple: Notarizing macOS software](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
- [Sparkle: Security and reliability](https://sparkle-project.org/documentation/security-and-reliability/).
