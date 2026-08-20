#include <rex/system/mod_plugin.h>

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version() {
  return rex::system::kModPluginAbiVersion + 1;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t, const rex::system::ModHostContext*) {
  return nullptr;
}
