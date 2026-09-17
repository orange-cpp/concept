#include "concept/compiler.hpp"
#include "concept/package.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#endif

namespace {

std::filesystem::path current_executable(const char* argv0) {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length != 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size != 0 && _NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::error_code error;
        auto resolved =
            std::filesystem::canonical(buffer.data(), error);
        if (!error) {
            return resolved;
        }
        return std::filesystem::absolute(buffer.data());
    }
#endif
    return std::filesystem::absolute(argv0);
}

// Locate the standard-library module root beside the compiler. Newer layouts
// stage the modules under "lib/concept" (so the directory never collides with
// the "concept" executable on platforms without an .exe extension); the legacy
// Windows layout keeps them directly under "concept".
std::filesystem::path module_root_for(
    const std::filesystem::path& compiler_directory) {
    const std::filesystem::path candidates[] = {
        compiler_directory / "lib" / "concept",
        compiler_directory / "concept",
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error) && !error) {
            return candidate;
        }
    }
    return candidates[0];
}

std::string read_source(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open source file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

void print_usage() {
    std::cerr << "usage: concept <source.concept> [-o <program.exe>] "
                 "[--runtime <runtime>] [--vms <1..64>] "
                 "[--shared-module]\n";
}

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 2;
        }

        const std::filesystem::path source_path = argv[1];
        auto output_path = source_path;
#ifdef _WIN32
        output_path.replace_extension(".exe");
#else
        output_path.replace_extension();
#endif
        const auto compiler_directory =
            current_executable(argv[0]).parent_path();
        std::filesystem::path runtime_path = compiler_directory /
#ifdef _WIN32
            "concept-runtime.exe";
#else
            "concept-runtime";
#endif
        std::uint32_t vm_count = 4;
        bool shared_module = false;
        // Only consulted in the Windows shared-module branch below.
        [[maybe_unused]] bool output_was_set = false;
        [[maybe_unused]] bool runtime_was_set = false;

        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if ((argument == "-o" || argument == "--runtime" ||
                 argument == "--vms") &&
                index + 1 >= argc) {
                throw std::runtime_error("missing path after " + argument);
            }
            if (argument == "-o") {
                output_path = argv[++index];
                output_was_set = true;
            } else if (argument == "--runtime") {
                runtime_path = argv[++index];
                runtime_was_set = true;
            } else if (argument == "--vms") {
                const std::string_view value = argv[++index];
                const auto result = std::from_chars(
                    value.data(), value.data() + value.size(), vm_count);
                if (result.ec != std::errc{} ||
                    result.ptr != value.data() + value.size() ||
                    vm_count == 0 || vm_count > 64) {
                    throw std::runtime_error(
                        "--vms must be an integer between 1 and 64");
                }
            } else if (argument == "--shared-module") {
                shared_module = true;
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }

        if (shared_module) {
#ifdef _WIN32
            if (!output_was_set) {
                output_path.replace_extension(".dll");
            }
            if (!runtime_was_set) {
                runtime_path =
                    compiler_directory / "concept-runtime-shared.dll";
            }
#else
            throw std::runtime_error(
                "--shared-module is currently supported only on Windows");
#endif
        }

        if (std::filesystem::absolute(source_path).lexically_normal() ==
            std::filesystem::absolute(output_path).lexically_normal()) {
            throw std::runtime_error("output path cannot overwrite the source file");
        }

        const auto source = read_source(source_path);
        const auto module_root = module_root_for(compiler_directory);
        const auto bytecode = cpt::compile(source, source_path.string(),
                                           vm_count, module_root.string(),
                                           shared_module
                                               ? cpt::CompileMode::shared_module
                                               : cpt::CompileMode::executable);
        const auto image = cpt::serialize(bytecode);
        cpt::package_executable(runtime_path, output_path, image);
        std::cout << "built " << std::filesystem::absolute(output_path).string()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "concept: " << error.what() << '\n';
        return 1;
    }
}
