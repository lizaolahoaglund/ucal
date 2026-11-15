#include "../include/UcalPcanViewSupport.h"
#include <windows.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return 3;
    const std::wstring executable(path, length);
    const auto slash = executable.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 3;
    const auto library = executable.substr(0, slash + 1u) + L"UcalPcanViewSupport.dll";
    const auto module = LoadLibraryExW(library.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        std::wcerr << L"PCAN-View support module could not be loaded: " << library << L"\n";
        return 3;
    }
    const auto run = reinterpret_cast<UcalPcanViewSupportRunFunctionV1>(
        GetProcAddress(module, "UcalPcanViewSupportRunV1"));
    if (!run) {
        std::wcerr << L"PCAN-View support module has no compatible entry point\n";
        FreeLibrary(module);
        return 3;
    }
    std::vector<std::vector<std::uint16_t>> strings;
    std::vector<const std::uint16_t*> arguments;
    strings.reserve(static_cast<std::size_t>(argc));
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        strings.emplace_back();
        for (const auto* character = argv[index]; *character; ++character)
            strings.back().push_back(static_cast<std::uint16_t>(*character));
        strings.back().push_back(0u);
        arguments.push_back(strings.back().data());
    }
    const auto result = run(static_cast<std::uint32_t>(argc), arguments.data());
    FreeLibrary(module);
    return static_cast<int>(result);
}
