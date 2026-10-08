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

## GitHub Actions release pipeline

`.github/workflows/release.yml` builds the six supported configurations on
standard GitHub-hosted runners. Linux builds run inside digest-pinned glibc
2.28 or Alpine/musl 1.2.5 images after checkout on the host runner; the ARM64
jobs use native ARM64 runners. Pull requests, pushes to `main`, and manual
dispatches run the build, tests, release-build checks, and upload CI artifacts.

Each platform job runs all 39 CTests with `BUILD_TESTING=ON`, then builds with
`BUILD_TESTING=OFF`, places those release libraries into the consumer test
directory, and runs the 31 applicable non-fault-injection tests against the
release binaries. Linux jobs also check the ELF architecture, GLIBC 2.28
symbol-version ceiling (glibc jobs), absence of dynamic libstdc++/libgcc,
and the exact factory/interface-RTTI export set. Windows checks x64 and the
static-CRT import boundary; macOS checks arm64, a deployment target of 11.0,
and system-only dynamic dependencies. Each archive contains both drivers,
configuration examples, project license files, a `SHA256SUMS` file, and
`BUILD-INFO.txt` with the source revision and toolchain/runtime details.

Only a pushed semantic version tag (`vMAJOR.MINOR.PATCH`, optionally with
pre-release/build metadata) can publish a GitHub Release. The publish job waits
for every matrix configuration, verifies the tag resolves to the checked-out
commit, requires exactly six platform archives, writes an aggregate
`SHA256SUMS`, creates a draft release, and publishes it. Existing releases are
left untouched and cause the job to fail; rerunning a published tag does not
replace its assets.
