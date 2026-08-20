#include <rex/system/mod_plugin.h>

#include <atomic>
#include <cstdint>

namespace {

std::atomic<uint32_t> g_created{0};
std::atomic<uint32_t> g_dialogs{0};
std::atomic<uint32_t> g_launched{0};
std::atomic<uint32_t> g_shutdown{0};

class TestModPlugin final : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override { ++g_dialogs; }
  void OnModuleLaunched() override { ++g_launched; }
  void OnShutdown() override { ++g_shutdown; }
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version() {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* context) {
  if (abi_version != rex::system::kModPluginAbiVersion || !context ||
      context->struct_size < sizeof(rex::system::ModHostContext)) {
    return nullptr;
  }
  ++g_created;
  return new TestModPlugin();
}

extern "C" REX_MOD_PLUGIN_EXPORT void rex_test_mod_counts(uint32_t* counts) {
  counts[0] = g_created.load();
  counts[1] = g_dialogs.load();
  counts[2] = g_launched.load();
  counts[3] = g_shutdown.load();
}
