# Building Wine for Android (ARM64)

This document provides detailed instructions for building Wine specifically for Android ARM64 environments.

## Prerequisites

### Development Environment

1. **Linux Build Host**
   
   While it's possible to build on macOS as you've done, a Linux host generally makes cross-compilation for Android easier.

2. **Required Packages**
   
   On Ubuntu/Debian:
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential flex bison gcc-mingw-w64 \
     gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
     libx11-dev libfreetype6-dev
   ```

### Android NDK

1. **Download Android NDK**
   ```bash
   mkdir -p android-ndk
   cd android-ndk
   wget -q https://dl.google.com/android/repository/android-ndk-r25c-linux.zip
   unzip -q android-ndk-r25c-linux.zip
   ```

2. **Set Environment Variables**
   ```bash
   export ANDROID_NDK_HOME=$PWD/android-ndk-r25c
   export PATH=$PATH:$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
   ```
   Consider adding these to your `.bashrc` or `.zshrc` for persistence.

## Building Wine

1. **Configure Wine for Android ARM64**
   ```bash
   ./configure --enable-win64
   ```

   Note: In many cases, this basic configuration is sufficient as the build system will detect your environment. If you need more specific options, you might add them based on your requirements.

2. **Build Wine**
   ```bash
   make -j$(nproc)
   ```

3. **Create a Package**
   ```bash
   mkdir -p wine-android-arm64
   cp -r * wine-android-arm64/ || true
   tar -czf wine-android-arm64.tar.gz wine-android-arm64
   ```

## Deploying to Android

1. **Install Termux**
   
   You'll need Termux installed on your Android device to run command-line applications.

2. **Transfer and Extract**
   
   Transfer the `wine-android-arm64.tar.gz` file to your Android device and extract it in Termux:
   ```bash
   tar -xzf wine-android-arm64.tar.gz
   ```

3. **Setting Up Environment**
   
   In Termux, you may need to set up the environment variables:
   ```bash
   export WINEPREFIX=$HOME/.wine
   ```

4. **Running Wine**
   ```bash
   ./wine notepad
   ```

## Common Issues and Solutions

### Missing Dependencies
If you encounter missing shared libraries on Android, you may need to install additional packages through Termux:
```bash
pkg install x11-repo
pkg install proot pulseaudio libx11 libgl
```

### Graphics Issues
For graphical applications, you'll need to set up an X server on Android. Consider using:
- VNC Server + VNC Viewer
- XServer XSDL
- Termux:X11

### Performance Optimization
- Use the `--compile-commands` flag with Wine for JIT compilation which may improve performance on ARM64

## Additional Resources

- [Wine on Android Wiki](https://wiki.winehq.org/Android)
- [Termux Wiki](https://wiki.termux.com/wiki/Main_Page)
- [Android NDK Documentation](https://developer.android.com/ndk/guides)

## Notes for macOS Users

If you're building on macOS for Android deployment:

1. Install the necessary cross-compiler tools:
   ```bash
   brew install llvm android-ndk
   ```

2. Set up environment variables appropriately:
   ```bash
   export ANDROID_NDK_HOME=$(brew --prefix)/share/android-ndk
   export PATH=$PATH:$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin
   ```

3. Follow the configuration and build steps above, but use `$(sysctl -n hw.ncpu)` instead of `$(nproc)` for the make parallelism. 