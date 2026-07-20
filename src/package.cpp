#include "concept/package.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace cpt {
namespace {

constexpr std::array<char, 8> package_magic{'C', 'P', 'T', 'P',
                                             'K', 'G', '0', '1'};
constexpr std::uint64_t trailer_size = 16;

void write_u64(std::ostream& output, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.put(static_cast<char>(value >> shift));
    }
}

std::uint64_t read_u64(std::istream& input) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        const auto byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            throw std::runtime_error("truncated Concept executable trailer");
        }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte))
                 << (index * 8);
    }
    return value;
}

} // namespace

void package_executable(const std::filesystem::path& runtime,
                        const std::filesystem::path& output,
                        const std::span<const std::uint8_t> payload) {
    if (!std::filesystem::is_regular_file(runtime)) {
        throw std::runtime_error("runtime stub does not exist: " +
                                 runtime.string());
    }

    const auto runtime_path = std::filesystem::absolute(runtime).lexically_normal();
    const auto output_path = std::filesystem::absolute(output).lexically_normal();
    if (runtime_path == output_path) {
        throw std::runtime_error("output path cannot overwrite the runtime stub");
    }

    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ifstream runtime_stream(runtime_path, std::ios::binary);
    if (!runtime_stream) {
        throw std::runtime_error("cannot open runtime stub: " +
                                 runtime_path.string());
    }

    std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot open output executable: " +
                                 output_path.string());
    }

    stream << runtime_stream.rdbuf();
    stream.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    stream.write(package_magic.data(),
                 static_cast<std::streamsize>(package_magic.size()));
    write_u64(stream, payload.size());
    if (!stream) {
        throw std::runtime_error("failed while writing output executable: " +
                                 output_path.string());
    }

#ifndef _WIN32
    std::error_code error;
    const auto permissions = std::filesystem::status(runtime_path).permissions();
    std::filesystem::permissions(output_path, permissions, error);
    if (error) {
        throw std::runtime_error("cannot mark output executable: " +
                                 error.message());
    }
#endif
}

std::vector<std::uint8_t>
read_embedded_payload(const std::filesystem::path& executable) {
    std::ifstream stream(executable, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("cannot open Concept executable: " +
                                 executable.string());
    }

    const auto end_position = stream.tellg();
    if (end_position < 0 ||
        static_cast<std::uint64_t>(end_position) < trailer_size) {
        throw std::runtime_error("executable has no Concept bytecode payload");
    }

    const auto file_size = static_cast<std::uint64_t>(end_position);
    stream.seekg(static_cast<std::streamoff>(file_size - trailer_size));
    std::array<char, package_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != package_magic) {
        throw std::runtime_error("executable has no Concept bytecode payload");
    }

    const auto payload_size = read_u64(stream);
    if (payload_size > file_size - trailer_size ||
        payload_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("invalid Concept executable payload size");
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size));
    stream.seekg(static_cast<std::streamoff>(file_size - trailer_size -
                                             payload_size));
    stream.read(reinterpret_cast<char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    if (!stream) {
        throw std::runtime_error("cannot read Concept executable payload");
    }
    return payload;
}

} // namespace cpt
