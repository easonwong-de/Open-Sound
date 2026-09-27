# OpenSound

A native macOS application for audio management and sound utilities.

## Development

### Prerequisites

- macOS 15.0 or later
- Xcode 16.0 or later
- [SwiftFormat](https://github.com/nicklockwood/SwiftFormat) (optional, for code formatting)

### Building and Running

1. Open `OpenSound.xcodeproj` in Xcode.
2. Select the `OpenSound` scheme and target **My Mac**.
3. Press `Cmd + R` to build and run.

### Code Formatting

Format Swift source code using the configuration defined in [`.swiftformat`](.swiftformat):

```bash
# Install SwiftFormat via Homebrew
brew install swiftformat

# Format all Swift files in the repository
swiftformat .

# Lint without applying modifications
swiftformat --lint .
```

## Licence

This project is licensed under the MIT Licence - see the [LICENSE](LICENSE) file for details.
