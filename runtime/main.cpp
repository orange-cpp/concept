#include "concept/bytecode.hpp"
#include "concept/package.hpp"
#include "concept/vm.hpp"

#include <xorstr.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
#endif
    return std::filesystem::absolute(argv0);
}

} // namespace

int main(int argc, char** argv) {
    try {
        static_cast<void>(argc);
        const auto payload = cpt::read_embedded_payload(current_executable(argv[0]));
        const auto bytecode = cpt::deserialize(payload);
        return static_cast<int>(cpt::execute(bytecode));
    } catch (const std::exception& error) {
        std::cerr << xorstr_("concept runtime: ") << error.what() << '\n';
        return 1;
    }
}
