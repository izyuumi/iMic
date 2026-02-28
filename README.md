# iMic

iMic is a pairing between an iPhone app that streams the microphone and a macOS host app that presents the stream as a virtual CoreAudio input device. The macOS host packs a driver bundle (`iMicDriver.driver`) that installs itself under `/Library/Audio/Plug-Ins/HAL/` when the app obtains administrator privileges.

## Repository layout

- `iMic/`: iOS SwiftUI app that captures audio and advertises `_imic._tcp` via Bonjour.
- `iMicHost/`: macOS SwiftUI host app that installs the driver, discovers the phone, and renders connection UI.
- `iMicDriver/`: CoreAudio HAL driver that reads from a shared ring buffer created by the phone.
- `Scripts/`: helper scripts for installing the driver system-wide and for release workflows.
- `Config/`: signing configuration (example and user-specific overrides).
- `.github/workflows/`: CI and release automation.

## Build prerequisites

- Xcode 26.3+ (macOS 14+ SDK)
- A paid Apple Developer account with:
  - `Developer ID Application` certificate for signing the macOS host
  - Notarization credentials (App Store Connect API key or Apple ID + app-specific password)
- `xcodebuild`, `xcrun notarytool`, `gh` (for release automation), `brew` (for tap validation)

## Signing setup

Run `./Scripts/setup-signing.sh` once after cloning to create `Config/Signing.xcconfig` from the example file. Populate that file with your `DEVELOPMENT_TEAM` and `CODE_SIGN_IDENTITY` before invoking `xcodebuild` or the release scripts. The release automation regenerates `Config/Signing.xcconfig` from the secrets you configure via `Scripts/release/generate-signing-config.sh`.

## Local development

1. Build the driver first: `xcodebuild -project iMic.xcodeproj -scheme iMicDriver -configuration Release`.
2. Build the macOS host: `xcodebuild -project iMic.xcodeproj -scheme iMicHost -configuration Release`.
3. Build the iOS app (for simulator or device): `xcodebuild -project iMic.xcodeproj -scheme iMic -configuration Release -destination 'generic/platform=iOS'`.
4. Run `iMicHost.app`, approve the driver install prompt, and observe the driver at System Settings > Sound > Input.

## Release workflow (macOS)

1. Tag a version: `git tag v<MARKETING_VERSION>`.
2. Run `Scripts/release/build-mac.sh`.
3. Export and package into `dist/iMicHost-<version>-macos.zip`.
4. Notarize via `Scripts/release/notarize-mac.sh`.
5. Attach the notarized ZIP to the GitHub Release.
6. Wait for the `update-tap` workflow to refresh `izyuumi/tap` with the new cask.
7. `Scripts/release/update-tap.sh` pushes the updated cask to `izyuumi/homebrew-tap` and opens a PR, so the release workflow needs `HOMEBREW_TAP_TOKEN` (or `GH_TOKEN`) with push rights to that repo.

## Homebrew tap

```sh
brew tap izyuumi/tap
brew install --cask izyuumi/tap/imic-host
```

The cask installs `iMicHost.app` only. On first launch the app will request admin privileges so it can install `iMicDriver` to `/Library/Audio/Plug-Ins/HAL/`. Homebrew does not install that driver for you.

## Security

Report vulnerabilities to [security@yourdomain.example](mailto:security@yourdomain.example) (replace with your real security contact) with timeline expectations.
