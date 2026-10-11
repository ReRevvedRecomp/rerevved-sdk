#include "companion_control.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace {
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

toml::array Words(const std::vector<std::uint32_t>& words) {
  toml::array result;
  for (auto word : words)
    result.push_back(std::int64_t(word));
  return result;
}

toml::table Draw(const rex::graphics::testing::CompanionDrawRecord& draw) {
  toml::array textures;
  for (const auto& texture : draw.textures) {
    textures.push_back(toml::table{
        {"stage", std::int64_t(texture.stage)},
        {"binding", std::int64_t(texture.shader_binding_index)},
        {"fetch", std::int64_t(texture.fetch_constant)},
        {"fetch_words", Words(texture.fetch_words)},
        {"signs", std::int64_t(texture.swizzled_signs)},
        {"signed", texture.is_signed},
        {"required", texture.required},
        {"available", texture.available},
        {"reason", std::int64_t(texture.unavailable_reason)},
        {"gpu_written", texture.gpu_written},
        {"memory_version_identity", std::to_string(texture.memory_version_identity)},
        {"memory_version_generation", std::to_string(texture.memory_version_generation)},
        {"width", std::int64_t(texture.width)},
        {"height", std::int64_t(texture.height)},
        {"depth", std::int64_t(texture.depth)},
        {"format", std::int64_t(texture.format)},
        {"pitch", std::int64_t(texture.pitch)},
        {"base_address", std::int64_t(texture.base_address)},
        {"base_size", std::int64_t(texture.base_size)},
        {"mip_address", std::int64_t(texture.mip_address)},
        {"mip_size", std::int64_t(texture.mip_size)},
        {"sampler", std::int64_t(texture.sampler_value)},
        {"component_mapping", std::int64_t(texture.component_mapping)}});
  }
  return toml::table{{"id", std::int64_t(draw.id)},
                     {"frame", std::int64_t(draw.frame)},
                     {"submission", std::int64_t(draw.submission)},
                     {"prepared", draw.prepared},
                     {"original_host_issued", draw.original_host_issued},
                     {"cpu_data_complete", draw.cpu_data_complete},
                     {"reason", std::int64_t(draw.unavailable_reason)},
                     {"execution_status", std::int64_t(draw.execution_status)},
                     {"execution_reason", std::int64_t(draw.execution_reason)},
                     {"vertex_hash", std::to_string(draw.vertex_shader_hash)},
                     {"pixel_hash", std::to_string(draw.pixel_shader_hash)},
                     {"registers", Words(draw.registers)},
                     {"vertex_words", Words(draw.vertex_shader_words)},
                     {"pixel_words", Words(draw.pixel_shader_words)},
                     {"textures", std::move(textures)}};
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    Require(argc == 2, "usage: rexruntime_installed_companion_control <fresh-result.json>");
    const std::filesystem::path output(argv[1]);
    Require(!std::filesystem::exists(output), "result path already exists");
    rex::graphics::testing::CompanionControlResult result;
    std::string error;
    const int status = rex::graphics::testing::RunCompanionControl(nullptr, result, error);
    toml::array prepared, original;
    for (const auto& draw : result.prepared_draws)
      prepared.push_back(Draw(draw));
    for (const auto& draw : result.original_draws)
      original.push_back(Draw(draw));
    toml::table report{{"mode", result.mode},
                       {"fixture_exit", status},
                       {"error", error},
                       {"progressed", result.progressed},
                       {"completion_observed", result.completion_observed},
                       {"source_fetch_words", Words(result.source_fetch_words)},
                       {"prepared", std::move(prepared)},
                       {"original", std::move(original)}};
    std::ofstream file(output);
    file << toml::json_formatter{report} << '\n';
    file.close();
    Require(bool(file), "cannot write result");
    Require(status == 0, error.empty() ? "fixture failed" : error.c_str());
    Require(result.prepared_draws.size() == 1 && result.original_draws.size() == 1,
            "expected one matching draw pair");
    const auto& before = result.prepared_draws.front();
    const auto& after = result.original_draws.front();
    Require(before.id == after.id && before.registers == after.registers &&
                before.vertex_shader_words == after.vertex_shader_words &&
                before.pixel_shader_words == after.pixel_shader_words,
            "original and Prepared raw identity differs");
    Require(!before.original_host_issued && before.execution_status == 0 &&
                after.original_host_issued && after.execution_status == 1,
            "original execution provenance differs");
    Require(before.textures.size() == 2 && after.textures.size() == 2,
            "expected ordered unsigned/signed descriptor pair");
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& a = before.textures[i];
      const auto& b = after.textures[i];
      Require(a.shader_binding_index == i && a.is_signed == (i == 1) && a.stage == b.stage &&
                  a.fetch_constant == b.fetch_constant && a.fetch_words == b.fetch_words &&
                  a.swizzled_signs == b.swizzled_signs && a.required == b.required,
              "companion identity or classification differs from original");
    }
    for (const auto& texture : before.textures) {
      if (!texture.required) {
        Require(!texture.memory_version_identity && !texture.memory_version_generation,
                "optional Prepared binding acquired a memory version");
      }
    }
    if (result.mode == "bc1") {
      const auto source_reason =
          static_cast<std::uint32_t>(rex::graphics::diagnostic::DrawCaptureReason::kTextureSource);
      Require(!before.cpu_data_complete && before.unavailable_reason == source_reason &&
                  before.textures[0].required && !before.textures[0].available &&
                  !before.textures[1].required && !before.textures[1].available &&
                  before.textures[1].unavailable_reason == source_reason,
              "BC1 coverage or optional failure reason changed");
    } else {
      Require(before.cpu_data_complete, "supported CPU draw is incomplete");
      const bool unsigned_required = result.mode != "signed";
      const bool signed_required = result.mode != "unsigned";
      Require(before.textures[0].required == unsigned_required &&
                  before.textures[1].required == signed_required,
              "signed or mixed demand lost a required binding");
    }
    std::cout << "installed companion control passed: " << result.mode << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
