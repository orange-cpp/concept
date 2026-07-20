#include "concept/bytecode.hpp"
#include "concept/package.hpp"
#include "concept/vm.hpp"

#include <xorstr.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::filesystem::path module_path(const HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error(
            xorstr_("cannot resolve Concept DLL module path"));
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool run_concept_entry(const HMODULE module) {
    try {
        const auto payload = cpt::read_embedded_payload(module_path(module));
        const auto bytecode = cpt::deserialize(payload);
        return cpt::execute(bytecode) != 0;
    } catch (const std::exception& error) {
        const auto message =
            std::string(xorstr_("concept DLL runtime: ")) + error.what();
        OutputDebugStringA(message.c_str());
        return false;
    }
}

char module_anchor;

DWORD WINAPI concept_worker(void* context) {
    const auto module = static_cast<HMODULE>(context);
    const bool success = run_concept_entry(module);
    if (!success) {
        OutputDebugStringA(
            xorstr_("concept DLL runtime: dll_main returned false"));
    }
    FreeLibraryAndExitThread(module, success ? 0 : 1);
}

} // namespace

extern "C" BOOL WINAPI DllMain(const HINSTANCE instance,
                               const DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    DisableThreadLibraryCalls(instance);

    HMODULE module_reference = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&module_anchor), &module_reference)) {
        return FALSE;
    }

    const auto thread = CreateThread(nullptr, 0, concept_worker,
                                     module_reference, 0, nullptr);
    if (thread == nullptr) {
        FreeLibrary(module_reference);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}
