# Distribution Plan

This project is moving from a local MVP to a public macOS app plus M5Stack
firmware. Do not treat the current repo-dependent developer install as a public
release artifact.

## Current State

`scripts/build-macos-companion` builds the Swift menu bar app and installs it to:

```text
/Applications/M5Stack Codex Companion.app
```

That is correct for local use on a developer Mac. It is not enough for public
distribution because the app currently stores `STICKS3RepoPath` in its
`Info.plist` and controls Python code from this checkout and `.bridge-venv`.
Other users should not need this repository, Xcode, PlatformIO, or a manually
created Python virtual environment to run the app.

Current public-release blockers:

- `codesign` reports `Signature=adhoc` and no `TeamIdentifier`.
- Gatekeeper assessment does not pass as a Developer ID-distributed app.
- The installed app contains a developer-machine `STICKS3RepoPath`.
- The bridge helper imports Python code from this checkout.
- Firmware flashing is not yet a guided app flow or a packaged release asset.

## Recommended Public Distribution

Use signed and notarized GitHub Release artifacts as the source of truth, then
offer Homebrew Cask as an install channel on top of those artifacts.

1. Primary artifact: `M5Stack-Codex-Companion-<version>.dmg`
2. Secondary artifact for Homebrew: the same DMG or a notarized zip
3. Homebrew channel: a cask in a project tap, for example
   `brew install --cask <tap>/m5stack-codex-companion`
4. Optional later: Sparkle update feed for users who install from the DMG

Homebrew Formula is not the right primary install type for the GUI app. Formulae
are better for command-line tools and build-from-source flows. The GUI app should
be a Cask because Homebrew Cask is designed to install `.app` bundles into
Applications.

## Release Readiness Requirements

### Self-contained macOS app

The app bundle must include everything needed at runtime:

- Swift menu bar executable
- Python bridge package
- Python runtime or a frozen bridge executable
- BLE dependencies, including PyObjC/Bleak dependencies
- Helper launcher resources
- Firmware binaries or a clear firmware download path

The production app must not rely on:

- A source checkout path
- `.bridge-venv`
- Homebrew Python
- Xcode or SwiftPM
- PlatformIO for normal users

### Helper and background process

The BLE-touching helper must remain a real app bundle with:

- `NSBluetoothAlwaysUsageDescription`
- `NSBluetoothPeripheralUsageDescription`
- Hardened runtime-compatible signing
- A stable bundle identifier
- A stable install location under
  `~/Library/Application Support/M5Stack Codex Companion/`

For a public app, prefer a first-run setup flow that installs or repairs the
helper and explains Bluetooth and Files/Folders prompts. Longer term, consider
`SMAppService` for a more native login-item flow instead of managing a raw
LaunchAgent plist directly.

### Signing and notarization

Public macOS distribution outside the Mac App Store requires:

- Apple Developer Program membership
- Developer ID Application certificate
- Hardened runtime
- Correct entitlements for the app and helper
- Recursive signing of nested code
- Notarization with `xcrun notarytool`
- Stapling with `xcrun stapler`
- Gatekeeper validation on a clean Mac account

Use ad-hoc signing only for local developer builds.

### Homebrew Cask

The cask should point to the versioned GitHub Release artifact and include:

- `version`
- `sha256`
- `url`
- `name`
- `desc`
- `homepage`
- `app "M5Stack Codex Companion.app"`
- `zap` entries for Application Support, LaunchAgents, logs, and preferences
- `depends_on macos:` matching the app's minimum supported macOS version

Do not put a repo-dependent build script behind a Homebrew cask. Homebrew should
install a prebuilt, signed, notarized app artifact.

## Firmware Distribution

The product includes device firmware, so releases should also publish firmware
artifacts:

- `sticks3-<version>.bin`
- `cardputer-adv-<version>.bin`
- board manifest with version, board type, BLE name prefix, and checksum

The app should eventually expose a firmware setup/update flow. Until then, the
release should include a clear flashing guide and checksums.

## Suggested Milestones

1. Remove repo-path runtime dependency.
   Bundle the bridge package and Python runtime/frozen bridge inside the app.

2. Production helper install.
   Generate or copy the BLE helper from bundled app resources, not from this
   repository.

3. Distribution signing.
   Add a `scripts/package-macos-release` path that signs with Developer ID,
   enables hardened runtime, builds a DMG, notarizes it, staples it, and verifies
   Gatekeeper.

4. Release automation.
   Add a GitHub Actions workflow for build/test/package once signing secrets and
   notarization credentials are available.

5. Homebrew tap.
   Publish a cask that installs the notarized release artifact.

6. Firmware release flow.
   Attach firmware binaries and checksums to the same GitHub Release, then add
   app-side setup/update UX.

## Validation Commands

For a release candidate:

```bash
codesign --verify --deep --strict "M5Stack Codex Companion.app"
spctl --assess --type execute --verbose=4 "M5Stack Codex Companion.app"
xcrun stapler validate "M5Stack-Codex-Companion-<version>.dmg"
shasum -a 256 "M5Stack-Codex-Companion-<version>.dmg"
```

For the installed app:

```bash
plutil -p "/Applications/M5Stack Codex Companion.app/Contents/Info.plist"
python3 scripts/sticks3-macos-bridge status
```

The second command remains developer-only until the bridge no longer depends on
this checkout.
