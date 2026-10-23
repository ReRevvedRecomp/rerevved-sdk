# SDK development

Build from a full repository checkout with its tracked submodules. Use the
consumer's exact SDK commit and installed version when reproducing a title
issue. An SDK build or artifact upload is not evidence that a title runs.

## Prerequisites

The [CMake project](../CMakeLists.txt) requires CMake 3.25 or newer, a 64-bit
target, and C++23. LLVM Clang must be version 18 or newer; AppleClang must be
version 16 or newer. The [presets](../CMakePresets.json) use Ninja Multi-Config.

- Windows x64: install LLVM Clang, Ninja, and Visual Studio C++ Build Tools with
  the Windows SDK. Run commands from an x64 developer environment.
- Linux: the supplied presets select `clang-22` and `clang++-22`. The [platform
  workflow](../.github/workflows/_build-platform.yaml) lists the matching
  system, Vulkan, windowing, and audio dependencies.
- macOS: the presets select Clang. The platform workflow uses Apple's compiler
  and installs CMake, Ninja, `vulkan-loader`, and `molten-vk`.
- Formatting: install Python and the pinned packages in
  [scripts/requirements-dev.txt](../scripts/requirements-dev.txt).
- PowerShell helpers: use PowerShell and import `scripts/PSReX`; direct CMake
  commands below do not need these wrappers.

Initialize the submodules declared in [`.gitmodules`](../.gitmodules) before
configuring. PowerPC instruction tests also need `powerpc-none-elf-as`,
`powerpc-none-elf-ld`, and `powerpc-none-elf-nm` in `tools/binutils/` (with
`.exe` on Windows). A source archive without submodule contents or those
binaries is not a complete test checkout.

## Build, test, and install on Windows

Run from the repository root:

```text
cmake --preset win-amd64 -DREXGLUE_BUILD_TESTS=ON
cmake --build --preset win-amd64-release
ctest --preset win-amd64-release --output-on-failure --no-tests=error
cmake --install out/build/win-amd64 --config Release
```

The installed prefix is `out/install/win-amd64`. Tests are off by default;
`REXGLUE_BUILD_TESTS=ON` enables unit tests and defaults PowerPC tests to on.
`--no-tests=error` prevents an empty test selection from looking like a pass.

For other platforms, replace `win-amd64` with the matching configure preset and
use its `-release` build and test presets. The artifact workflow covers
`win-amd64`, `linux-amd64`, `linux-arm64`, `mac-amd64`, and `mac-arm64`. A
`win-arm64` preset also exists, but is not in that artifact workflow.

The Unix artifact jobs configure with `REXGLUE_BUILD_PPC_TESTS=OFF` and run only
the selected mod plugin tests. To match that configuration, add the option
during configure and use:

```text
ctest --preset <platform>-release -R '^mod_plugin\.' --output-on-failure --no-tests=error
```

A focused test pass does not replace the full applicable suite. Run the full
matching CTest preset for behavior changes, and the mod-plugin selection on each
affected supported Unix platform for loading changes. Record any tests not run
and why.

## Check the changed surface

- Documentation: inspect relative links and anchors, check ASCII and trailing
  whitespace, and run `git diff --check` in a checkout. No build is needed for
  prose-only changes.
- C++: follow the [style guide](style-guide.md#verification). The [format
  workflow](../.github/workflows/lint.yaml) checks changed maintained C/C++
  files with the pinned formatter.
- Network behavior: after configuring and building tests, use the focused
  selection below, then the full applicable suite. Loopback tests verify SDK
  transport behavior. They do not establish that title discovery or play over
  the Internet works. Default test runs bind only loopback addresses. Cases that
  bind wildcard or other non-loopback addresses carry the hidden `[.network]`
  tag and are registered only when you configure with
  `-DREXGLUE_NETWORK_TESTS=ON` (CI does); expect a Windows Firewall prompt when
  running them.

```text
ctest --preset win-amd64-release -R "relay|invite|system_link|lan|[Ll]ive|Discord" --output-on-failure --no-tests=error
```

- Installed SDK surface: the [CPU
  consumer](../tests/installed_cpu_consumer/README.md) checks the installed
  runtime without source injection. Its optional plugin ABI check needs a
  Windows plugin built with both D3D12 and Vulkan, unlike the default Windows
  configuration. The check does not initialize a renderer or validate title
  runtime behavior.
- Public API or ABI: rebuild the SDK and consumers, validate the installed
  package, and follow [maintenance policy](MAINTAINING.md) before repinning.

## PowerShell wrappers

```powershell
Import-Module .\scripts\PSReX
```

`rex-configure` invokes the platform preset without enabling tests. `rex-test
-Config Release` invokes CTest without `--no-tests=error` or
`--output-on-failure`. `rex-install` installs Debug, Release, and
RelWithDebInfo, so all three configurations must exist. Use the explicit
commands above for a Release-only validation and install.

## Artifacts and consumer locks

[Maintaining the fork](MAINTAINING.md#artifacts-and-releases) defines how to
build artifacts from an exact commit and when they may be published. Validate
the consumer against the installed package before changing its lock. Keep SDK
outputs under `out/`; do not commit build products or machine configuration.
