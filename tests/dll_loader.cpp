#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>

int main(const int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path module_path(argv[1]);
    const auto module = LoadLibraryW(module_path.c_str());
    if (module == nullptr) {
        return 1;
    }
    if (!FreeLibrary(module)) {
        return 3;
    }

    const auto module_name = module_path.filename().wstring();
    constexpr unsigned maximum_wait_iterations = 30000;
    for (unsigned iteration = 0; iteration < maximum_wait_iterations;
         ++iteration) {
        if (GetModuleHandleW(module_name.c_str()) == nullptr) {
            return 42;
        }
        Sleep(10);
    }
    return 4;
}
