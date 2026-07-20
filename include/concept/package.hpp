#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace cpt {

void package_executable(const std::filesystem::path& runtime,
                        const std::filesystem::path& output,
                        std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t>
read_embedded_payload(const std::filesystem::path& executable);

} // namespace cpt
