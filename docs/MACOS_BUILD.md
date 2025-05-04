# Building Wine on macOS

This document provides detailed instructions for building Wine on macOS, with specific focus on Apple Silicon (ARM64) Macs.

## Prerequisites

### Required Software
1. **Xcode Command Line Tools**
   ```
   xcode-select --install
   ```

2. **Homebrew**
   ```
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install.sh)"
   ```

3. **Required packages**
   ```
   brew install mingw-w64 gcc bison flex
   ```

### LLVM MinGW Toolchain

1. **Download the LLVM MinGW toolchain**
   ```
   curl -L -o llvm-mingw.zip https://github.com/mstorsjo/llvm-mingw/releases/download/20250430/llvm-mingw-20250430-ucrt-macos-universal.zip
   unzip llvm-mingw.zip
   ```

2. **Add the toolchain to your PATH**
   ```
   export PATH=$PATH:/path/to/llvm-mingw-20250430-ucrt-macos-universal/bin
   ```
   Consider adding this to your shell profile (`.zshrc` or `.bash_profile`) for persistence.

## Setup libgcc.a

The LLVM MinGW toolchain needs `libgcc.a` for building Wine. We'll create a symbolic link to the Homebrew version:

1. **Determine your GCC version**
   ```
   GCC_VERSION=$(brew info gcc | grep -o '[0-9]*\.[0-9]*\.[0-9]*_[0-9]*' | head -1)
   ```

2. **Determine your Darwin version for the GCC build**
   ```
   GCC_DARWIN_VERSION=$(ls -d /opt/homebrew/Cellar/gcc/$GCC_VERSION/lib/gcc/current/gcc/aarch64-apple-darwin* | xargs basename)
   ```

3. **Create the lib directory in the MinGW toolchain**
   ```
   mkdir -p /path/to/llvm-mingw-20250430-ucrt-macos-universal/aarch64-w64-mingw32/lib
   ```

4. **Create the symbolic link**
   ```
   ln -s /opt/homebrew/Cellar/gcc/$GCC_VERSION/lib/gcc/current/gcc/$GCC_DARWIN_VERSION/$GCC_VERSION/libgcc.a /path/to/llvm-mingw-20250430-ucrt-macos-universal/aarch64-w64-mingw32/lib/libgcc.a
   ```

## Building Wine

1. **Configure Wine**
   ```
   ./configure
   ```
   
   Note: In many cases, no additional options are needed as the configure script will auto-detect your environment. However, if you encounter issues or need specific features, you can try the following options:
   ```
   ./configure --enable-win64 --with-mingw=aarch64-w64-mingw32
   ```

2. **Build**
   ```
   make -j$(sysctl -n hw.ncpu)
   ```

3. **Install (optional)**
   ```
   make install
   ```

## Common Issues and Solutions

### Missing libgcc.a
If you get an error like `clang: error: no such file or directory: 'libgcc.a'`, ensure you've created the symbolic link correctly. Check that the path to the gcc installation is correct:
```
ls -la /opt/homebrew/Cellar/gcc/
```

### Invalid arch in universal binary
This can happen if the toolchain doesn't match your system architecture. Make sure you're using the universal binary version of the LLVM MinGW toolchain.

### Can't find MinGW headers
If configure can't find your MinGW installation, make sure the PATH is set correctly and the `--with-mingw` option points to the correct toolchain prefix.

## Testing Your Build

After a successful build, you can test by running the Wine binary:
```
./wine --version
```

Or try running a simple Windows program:
```
./wine notepad
```

## Additional Resources

- [Wine on macOS Wiki](https://wiki.winehq.org/MacOS)
- [LLVM MinGW Project](https://github.com/mstorsjo/llvm-mingw)
- [Wine Compilation Guide](https://wiki.winehq.org/Building_Wine) 