#include <rex/system/mod_script.h>

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

#include <rex/runtime.h>
#include <rex/system/mod_catalog.h>

namespace rex {
system::ModScriptActivation Runtime::PrepareModScriptActivation(std::string_view id, size_t order) {
  active_mod_states_.reserve(active_mod_states_.size() + 1);
  return system::ModScriptActivation(*this, std::string(id), order);
}

namespace system {
ModScriptActivation::ModScriptActivation(Runtime& runtime, std::string id, size_t order)
    : runtime_(&runtime), id_(std::move(id)), order_(order) {}
ModScriptActivation::ModScriptActivation(ModScriptActivation&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)),
      id_(std::move(other.id_)),
      order_(other.order_) {}

void ModScriptActivation::Commit() noexcept {
  if (!runtime_)
    return;
  auto& states = runtime_->active_mod_states_;
  runtime_->mod_catalog_.SetActive(id_, true, order_);
  runtime_->failed_mod_messages_.erase(id_);
  const auto existing = std::find_if(states.begin(), states.end(),
                                     [this](const auto& state) { return state.id == id_; });
  if (existing != states.end())
    existing->order = order_;
  else
    states.push_back({std::move(id_), order_});  // Capacity and owned ID were prepared.
  runtime_ = nullptr;
}

namespace {
// Reporting allocates; if it cannot, the package simply stays inactive and
// dispatch continues with later packages before the guest resumes.
template <typename MakeMessage>
void ReportFailure(Runtime& runtime, std::string_view id, MakeMessage&& make_message) noexcept {
  try {
    runtime.MarkModLoadFailed(id, make_message());
  } catch (...) {}
}
}  // namespace

void DispatchModLaunch(Runtime& runtime, std::span<const ModPackage> packages,
                       const std::function<void(const ModPackage&)>& launch_native) {
  for (size_t order = 0; order < packages.size(); ++order) {
    const auto& package = packages[order];
    const bool native = package.payload_kind == ModPayloadKind::kNative;
    try {
      if (native) {
        launch_native(package);
        continue;
      }
      auto* host = runtime.mod_script_host();
      if (!host) {
        ReportFailure(runtime, package.id,
                      [&] { return "support: " + package.entry + ": no script host"; });
        continue;
      }
      auto result = RunModScriptFile(package.mod_root, package.entry);
      if (!result.ok) {
        ReportFailure(runtime, package.id, [&] {
          return std::string(ToString(result.error.stage)) + ": " + package.entry + ": " +
                 result.error.message;
        });
        continue;
      }
      auto activation = runtime.PrepareModScriptActivation(package.id, order);
      auto published = host->Publish({package.id, package.entry, package.api, package.api_version,
                                      package.required_domains, result.value});
      if (published.published) {
        activation.Commit();
      } else {
        ReportFailure(runtime, package.id, [&] {
          return std::string(ToString(published.stage)) + ": " + package.entry + ": " +
                 published.message;
        });
      }
    } catch (const std::bad_alloc&) {
      ReportFailure(runtime, package.id, [&] {
        return native ? std::string("launch: out of memory")
                      : "memory: " + package.entry + ": activation preparation failed";
      });
    } catch (const std::exception& error) {
      ReportFailure(runtime, package.id, [&] {
        return (native ? std::string("launch: ") : "publish: " + package.entry + ": ") +
               error.what();
      });
    } catch (...) {
      ReportFailure(runtime, package.id, [&] {
        return std::string(native ? "launch" : "publish") + ": unknown package callback failure";
      });
    }
  }
}
}  // namespace system
}  // namespace rex
