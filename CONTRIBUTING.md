## Contributing

### Development workflow

1. Open the workspace using Xcode.
2. Run `xcodebuild` for the target you are working on (Host, Driver, or iOS).
3. Keep the driver and host builds in sync with the shared ring buffer buffer layout before changing interfaces.

### Testing

- `xcodebuild -project iMic.xcodeproj -scheme iMicHost -configuration Release`
- `xcodebuild -project iMic.xcodeproj -scheme iMicDriver -configuration Release`
- `xcodebuild -project iMic.xcodeproj -scheme iMic -configuration Release -destination 'generic/platform=iOS'`

### Releases

- The repo relies on `Scripts/release/*` for signing, packaging, and notarization.
- A version tag must follow `v<MARKETING_VERSION>`.
- The release workflow will update the Homebrew tap automatically; just ensure `Scripts/release/{build,package,notarize,check}-mac.sh` all succeed.
