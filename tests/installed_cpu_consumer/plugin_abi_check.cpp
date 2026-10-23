#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

#include <rex/platform.h>
#include <rex/platform/dynlib.h>
#include <rex/system/gpu_plugin.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: rexruntime_plugin_abi_check <absolute-plugin-path>\n";
    return 1;
  }
  const std::filesystem::path plugin_path(argv[1]);

  rex::platform::DynamicLibrary library;
  if (!library.Load(plugin_path, rex::platform::SymbolResolution::kImmediate)) {
    std::cerr << "staged xenos plugin could not be loaded\n";
    return 1;
  }
  auto abi_version =
      library.GetSymbol<rex::system::GpuAbiVersionFn>(rex::system::kGpuAbiVersionSymbol);
  auto create = library.GetSymbol<rex::system::GpuCreateFn>(rex::system::kGpuCreateSymbol);
  if (!abi_version || !create || abi_version() != rex::system::kGpuPluginAbiVersion) {
    std::cerr << "staged xenos plugin exports the wrong ABI surface\n";
    return 1;
  }

  rex::system::GpuCreateInfo info{};
  info.struct_size = sizeof(info);
  info.backend = "any";
  if (create(rex::system::kGpuPluginAbiVersion + 1, &info) != nullptr ||
      create(rex::system::kGpuPluginAbiVersion, nullptr) != nullptr) {
    std::cerr << "xenos factory accepted an invalid ABI or null descriptor\n";
    return 1;
  }
  rex::system::GpuCreateInfo truncated = info;
  truncated.struct_size = sizeof(truncated) - 1;
  if (create(rex::system::kGpuPluginAbiVersion, &truncated) != nullptr) {
    std::cerr << "xenos factory accepted a truncated descriptor\n";
    return 1;
  }

  auto create_backend = [&](std::string_view backend) {
    rex::system::GpuCreateInfo backend_info = info;
    backend_info.backend = backend.data();
    return std::unique_ptr<rex::system::IGraphicsSystem>(
        create(rex::system::kGpuPluginAbiVersion, &backend_info));
  };
  auto d3d12 = create_backend("d3d12");
  auto vulkan = create_backend("vulkan");
  if (!d3d12 || !vulkan) {
    std::cerr << "xenos factory did not construct both backend systems\n";
    return 1;
  }
  d3d12->Shutdown();
  vulkan->Shutdown();
  return 0;
}
