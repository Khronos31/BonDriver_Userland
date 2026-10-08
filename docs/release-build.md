# Release builds

Build artifacts with `BUILD_TESTING=OFF`. The default release uses the compiler's
standard C++ runtime linkage. Run CMake from a toolchain environment matching
the intended target; choosing a newer compiler on a newer host does not, by
itself, lower the libc ABI baseline.

## Linux

For glibc distributions at the supported minimum, use a compiler and sysroot
based on glibc 2.28 or older, and verify the resulting dynamic symbol versions
against that sysroot. For musl, use a musl-targeting compiler/sysroot based on
musl 1.2.5. Build separately for x64 and arm64.

```sh
cmake -S . -B build-linux \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel 2
```

When using a cross compiler, pass its CMake toolchain file and the matching
sysroot, for example `-DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake`. Do not
reuse a build directory between libc families or architectures.

An optional GNU-compatible static C++/unwind runtime is available for Linux
ELF builds:

```sh
cmake -S . -B build-linux-static-cxx \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DBONDRIVER_STATIC_CXX_RUNTIME=ON
cmake --build build-linux-static-cxx --parallel 2
```

This option is supported with GNU or Clang Linux ELF toolchains. It statically
links libstdc++ and libgcc/unwind support into each driver and applies the
project export map so implementation/runtime symbols remain hidden while the
BonDriver factories and interface RTTI remain available. libc and other system
dependencies remain dynamic. Validate both DSOs in the target runtime and in
both load orders before distribution; this option does not make a glibc build
usable on musl or change the selected libc ABI baseline.

## Windows x64

The default uses the toolchain's normal MSVC runtime. To build product DLLs
with the static MSVC CRT, configure a separate release directory:

```powershell
cmake -S . -B build-windows-static-crt `
  -DBUILD_TESTING=OFF `
  -DBONDRIVER_STATIC_CRT=ON
cmake --build build-windows-static-crt --config Release --parallel 2
```

`BONDRIVER_STATIC_CRT` affects the two product DLLs. It is off by default and
does not change a separately built consumer's runtime setting. Keep allocation
and destruction of driver-owned objects inside the DLL through the factory and
`Release()` interface.

## macOS arm64

Build natively with the macOS SDK and AppleClang. Set the deployment target
explicitly so the output does not inherit the build host's newer default:

```sh
cmake -S . -B build-macos \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos --parallel 2
```

The deployment-target-11.0 build and independent ABI consumers were checked on
macOS 26. Runtime behavior on macOS 11 itself has not been tested. The Linux
static C++ runtime option does not apply to macOS.

All configurations produce `BonDriver_Siano` and `BonDriver_PX4` shared
libraries in the build tree. The supported output names are `.so` on Linux,
`.dll` on Windows, and `.dylib` on macOS.
