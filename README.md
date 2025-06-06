## INTRODUCTION

Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x, Win32, and Win64 executables) on Unix.
It consists of a program loader which loads and executes a Microsoft
Windows binary, and a library (called Winelib) that implements Windows
API calls using their Unix, X11 or Mac equivalents.  The library may also
be used for porting Windows code into native Unix executables.

Wine is free software, released under the GNU LGPL; see the file
LICENSE for the details.


## QUICK START

From the top-level directory of the Wine source (which contains this file),
run:

```
./configure
make
```

Then either install Wine:

```
make install
```

Or run Wine directly from the build directory:

```
./wine notepad
```

Run programs as `wine program`. For more information and problem
resolution, read the rest of this file, the Wine man page, and
especially the wealth of information found at https://www.winehq.org.


## REQUIREMENTS

To compile and run Wine, you must have one of the following:

- Linux version 2.0.36 or later
- FreeBSD 12.4 or later
- Solaris x86 9 or later
- NetBSD-current
- Mac OS X 10.8 or later

As Wine requires kernel-level thread support to run, only the operating
systems mentioned above are supported.  Other operating systems which
support kernel threads may be supported in the future.

**FreeBSD info**:
  See https://wiki.freebsd.org/Wine for more information.

**Solaris info**:
  You will most likely need to build Wine with the GNU toolchain
  (gcc, gas, etc.). Warning : installing gas does *not* ensure that it
  will be used by gcc. Recompiling gcc after installing gas or
  symlinking cc, as and ld to the gnu tools is said to be necessary.

**NetBSD info**:
  Make sure you have the USER_LDT, SYSVSHM, SYSVSEM, and SYSVMSG options
  turned on in your kernel.

**Mac OS X info**:
  You need Xcode/Xcode Command Line Tools or Apple cctools.  The
  minimum requirements for compiling Wine are clang 3.8 with the
  MacOSX10.10.sdk and mingw-w64 v8.  The MacOSX10.14.sdk and later can
  only build wine64.

**Mac OS X ARM64 (Apple Silicon) info**:
  For building Wine on Apple Silicon (ARM64) Macs, you need to:

  1. Install the LLVM MinGW toolchain for cross-compilation:
     - Download from https://github.com/mstorsjo/llvm-mingw/releases/download/20250430/llvm-mingw-20250430-ucrt-macos-universal.tar.xz
     - Add the toolchain bin directory to your PATH

  2. Install required dependencies via Homebrew:
     ```
     brew install mingw-w64 gcc
     ```

  3. Create a symbolic link for libgcc.a in your MinGW toolchain:
     ```
     mkdir -p /path/to/llvm-mingw/aarch64-w64-mingw32/lib
     ln -s /opt/homebrew/Cellar/gcc/VERSION/lib/gcc/current/gcc/aarch64-apple-darwin*/VERSION/libgcc.a /path/to/llvm-mingw/aarch64-w64-mingw32/lib/libgcc.a
     ```
     Replace VERSION with your gcc version (e.g., 14.2.0_1)

  4. Configure and build Wine:
     ```
     ./configure
     make
     ```
     
     Note: In many cases, no additional options are needed. However, if you encounter issues, you might need to specify options:
     ```
     ./configure --enable-win64 --with-mingw=aarch64-w64-mingw32
     ```

  For more detailed instructions, see [docs/MACOS_BUILD.md](docs/MACOS_BUILD.md).

**Android ARM64 info**:
  For building Wine to run on Android ARM64 environments:

  1. Install the Android NDK and required cross-compilation tools:
     ```
     sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
     ```

  2. Download and set up the Android NDK:
     ```
     export ANDROID_NDK_HOME=/path/to/android-ndk
     export PATH=$PATH:$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
     ```

  3. Configure and build Wine:
     ```
     ./configure --enable-win64
     make
     ```

  For more detailed instructions, including how to deploy and run on Android, see [docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md).

**Winlator Info**:
  For using Wine builds with Winlator (an Android application for running Windows software):

  1. You can build Wine for two different architectures:
     - ARM64 (native performance on ARM64 devices)
     - x86_64 (runs through Box64 translation layer)

  2. Cross-compilation requires a two-step process:
   
     a. First, build the native Wine tools:
     ```
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
     
     b. Then build for your target architecture:
     ```
     # For ARM64:
     ./configure --host=aarch64-w64-mingw32 --with-wine-tools=$PWD/wine-tools --enable-win64 --without-oss --disable-winemenubuilder --disable-tests
     
     # For x86_64:
     ./configure --host=x86_64-w64-mingw32 --with-wine-tools=$PWD/wine-tools --enable-win64 --without-oss --disable-winemenubuilder --disable-tests
     
     # Then build:
     make
     ```

  3. Package the build:
     ```
     mkdir -p wine-build
     cp -r * wine-build/ || true
     tar -czf wine-build.tar.gz wine-build
     ```

  For detailed instructions on using custom Wine builds with Winlator, see [docs/WINLATOR_GUIDE.md](docs/WINLATOR_GUIDE.md).

**Supported file systems**:
  Wine should run on most file systems. A few compatibility problems
  have also been reported using files accessed through Samba. Also,
  NTFS does not provide all the file system features needed by some
  applications.  Using a native Unix file system is recommended.

**Basic requirements**:
  You need to have the X11 development include files installed
  (called xorg-dev in Debian and libX11-devel in Red Hat).
  Of course you also need make (most likely GNU make).
  You also need flex version 2.5.33 or later and bison.

**Optional support libraries**:
  Configure will display notices when optional libraries are not found
  on your system. See https://wiki.winehq.org/Recommended_Packages for
  hints about the packages you should install. On 64-bit platforms,
  you have to make sure to install the 32-bit versions of these
  libraries.


## COMPILATION

To build Wine, do:

```
./configure
make
```

This will build the program "wine" and numerous support libraries/binaries.
The program "wine" will load and run Windows executables.
The library "libwine" ("Winelib") can be used to compile and link
Windows source code under Unix.

To see compile configuration options, do `./configure --help`.

For more information, see https://wiki.winehq.org/Building_Wine


## SETUP

Once Wine has been built correctly, you can do `make install`; this
will install the wine executable and libraries, the Wine man page, and
other needed files.

Don't forget to uninstall any conflicting previous Wine installation
first.  Try either `dpkg -r wine` or `rpm -e wine` or `make uninstall`
before installing.

Once installed, you can run the `winecfg` configuration tool. See the
Support area at https://www.winehq.org/ for configuration hints.


## RUNNING PROGRAMS

When invoking Wine, you may specify the entire path to the executable,
or a filename only.

For example, to run Notepad:

```
wine notepad            (using the search Path as specified in
wine notepad.exe         the registry to locate the file)

wine c:\\windows\\notepad.exe      (using DOS filename syntax)

wine ~/.wine/drive_c/windows/notepad.exe  (using Unix filename syntax)

wine notepad.exe readme.txt          (calling program with parameters)
```

Wine is not perfect, so some programs may crash. If that happens you
will get a crash log that you should attach to your report when filing
a bug.


## GETTING MORE INFORMATION

- **WWW**: A great deal of information about Wine is available from WineHQ at
	https://www.winehq.org/ : various Wine Guides, application database,
	bug tracking. This is probably the best starting point.

- **FAQ**: The Wine FAQ is located at https://www.winehq.org/FAQ

- **Wiki**: The Wine Wiki is located at https://wiki.winehq.org

- **Gitlab**: Wine development is hosted at https://gitlab.winehq.org

- **Mailing lists**:
	There are several mailing lists for Wine users and developers;
	see https://www.winehq.org/forums for more information.

- **Bugs**: Report bugs to Wine Bugzilla at https://bugs.winehq.org
	Please search the bugzilla database to check whether your
	problem is already known or fixed before posting a bug report.

- **IRC**: Online help is available at channel `#WineHQ` on irc.libera.chat.
