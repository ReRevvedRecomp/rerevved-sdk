# Installed CPU consumer

This standalone project proves that an installed SDK exposes the common
command decoder and register file through rex::runtime. It has no SDK source
injection and the CPU executable links only rex::runtime after
find_package(rexglue 0.24.0 REQUIRED CONFIG).

Configure it with an installed SDK prefix that satisfies the 0.24.0 minimum. For a multi-config
generator:

    cmake -S tests/installed_cpu_consumer -B out/installed_cpu_consumer -DCMAKE_PREFIX_PATH=C:/path/to/rexglue
    cmake --build out/installed_cpu_consumer --config Release
    out/installed_cpu_consumer/Release/rexruntime_cpu_consumer.exe

For a single-config generator, add -DCMAKE_BUILD_TYPE=Release during
configure, omit --config Release during build, and run
out/installed_cpu_consumer/rexruntime_cpu_consumer.

On Windows, stage the installed runtime DLLs beside the CPU executable or add
their directory to PATH. The CPU executable does not load a GPU plugin or
initialize a device. For the separate ABI check, stage the matching
configuration's rexgpu-xenos plugin DLL beside rexruntime_plugin_abi_check
along with its required DLL dependencies, then pass its absolute path to the
checker:

    out/installed_cpu_consumer/Release/rexruntime_plugin_abi_check.exe C:/path/to/rexgpu-xenos.dll

The checker validates the ABI 4 export and factory descriptor checks and
constructs both backend objects without setup. It requires a Windows plugin
built with both `REXGLUE_USE_D3D12=ON` and `REXGLUE_USE_VULKAN=ON`; the default
Windows SDK build has Vulkan off and cannot pass this separate check. The CPU
consumer itself does not require both backends.
