# Using Wine with Winlator on Android

This guide explains how to use custom Wine builds with [Winlator](https://github.com/brunodev85/winlator), an Android application that runs Windows applications using Wine and Box86/Box64.

## Overview

Winlator uses Box86 and Box64 to run x86 and x64 binaries on ARM devices. This allows running Wine (and subsequently Windows applications) on Android devices. By providing custom Wine builds, you can optimize performance or add specific features not available in the standard Winlator Wine distribution.

## Supported Architectures

Winlator supports two main architectures:

1. **x86_64 (64-bit)** - For running 64-bit Windows applications
2. **ARM64** - Native ARM64 builds that may offer better performance on ARM64 devices

## Getting Started

### Prerequisites

- An Android device with Winlator installed
- A custom Wine build for either x86_64 or ARM64

### Installing Your Custom Wine Build in Winlator

1. **Transfer the Wine package to your Android device**
   - Copy the `wine-arm64.tar.gz` or `wine-x86_64.tar.gz` file to your Android device

2. **Import the custom Wine build in Winlator**
   - Open Winlator
   - Go to Settings
   - Scroll to "Custom Wine Builds"
   - Tap "Import Wine Build"
   - Navigate to your Wine package and select it
   - Follow the on-screen instructions to complete the import

3. **Select your custom Wine build**
   - When creating a new container or editing an existing one
   - Under "Wine Build," select your imported custom build
   - Save the container settings

## Building for Optimal Performance

Cross-compiling Wine requires a two-step process: first building the native tools, then using those tools to build the target architecture.

### Step 1: Build Native Wine Tools

```bash
# Create a directory for the native tools
mkdir -p wine-tools
cp -r * wine-tools/ || true
cd wine-tools

# Configure and build native tools
./configure --enable-win64
make -j$(nproc)

# Return to the main directory
cd ..
```

### Step 2: Build for Your Target Architecture

#### ARM64 Native Builds

Native ARM64 builds generally offer better performance on ARM64 devices by avoiding the overhead of translation through Box64. However, compatibility may vary.

```bash
./configure --host=aarch64-w64-mingw32 --with-wine-tools=$PWD/wine-tools --enable-win64 --without-oss --disable-winemenubuilder --disable-tests
make -j$(nproc)
```

#### x86_64 Builds for Box64 Translation

When running through Box64, a standard x86_64 build of Wine is translated to ARM64 instructions at runtime. This approach offers better compatibility but potentially lower performance.

```bash
./configure --host=x86_64-w64-mingw32 --with-wine-tools=$PWD/wine-tools --enable-win64 --without-oss --disable-winemenubuilder --disable-tests
make -j$(nproc)
```

### Step 3: Package Your Build

```bash
mkdir -p wine-build
cp -r * wine-build/ || true
tar -czf wine-build.tar.gz wine-build
```

## Configuration Options Explained

The build configuration includes several important flags:

- `--host=aarch64-w64-mingw32` or `--host=x86_64-w64-mingw32`: Specifies the target architecture
- `--with-wine-tools=$PWD/wine-tools`: Points to the directory with native Wine tools (required for cross-compilation)
- `--enable-win64`: Builds 64-bit Wine (required for most modern Windows applications)
- `--without-oss`: Disables OSS audio support which is not needed on Android
- `--disable-winemenubuilder`: Disables the Wine menu builder which is not needed in Winlator
- `--disable-tests`: Speeds up build time by skipping test compilation

## Troubleshooting

### Common Issues

1. **Wine crashes on startup**
   - Check Winlator logs for specific errors
   - Try resetting the Wine prefix in container settings

2. **Poor performance**
   - Enable DXVK in Winlator container settings
   - Try switching between ARM64 and x86_64 builds to find optimal performance

3. **Application doesn't start**
   - Make sure your Wine build includes the necessary dependencies
   - Check compatibility with your specific Wine version

4. **Cross-compilation errors**
   - Ensure you've built the native tools first
   - Verify the `--with-wine-tools` path is correct
   - Check that you have the appropriate cross-compiler installed

### Viewing Logs

To view logs in Winlator:
1. Go to the "Logs" tab in Winlator
2. Select your container
3. Review the logs for any error messages

## Advanced Configuration

### Box64 Settings

Box64 configuration can significantly impact performance:

1. **JIT Cache Size**
   - In Winlator settings, increase JIT cache for better performance

2. **Dynarec Options**
   - Enable/disable specific dynarec options in Box64 settings

### Wine Registry Tweaks

You can adjust Wine registry settings within Winlator:
1. Open the container
2. Run "Wine Registry Editor" from the Winlator menu
3. Modify registry settings like graphics drivers or DLL overrides

## Resources

- [Winlator GitHub Repository](https://github.com/brunodev85/winlator)
- [Box86/Box64 Documentation](https://github.com/ptitSeb/box86)
- [Wine Wiki](https://wiki.winehq.org/)

## Contributing

If you develop an optimized Wine build for Winlator, consider sharing your configurations and optimizations with the community to help others achieve better performance on Android devices. 