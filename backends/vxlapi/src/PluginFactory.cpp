#include "VectorBackend.hpp"
#include <windows.h>
std::unique_ptr<Ucal::Backend::BackendInterface> makeUcalPlugin(const std::wstring& configuration) {
    if (!configuration.empty()) {
        wchar_t value[32768]{};
        GetPrivateProfileStringW(L"runtime", L"library", L"", value, 32768, configuration.c_str());
        if (*value) {
            std::wstring library(value);
            if (library.size() < 3 || library[1] != L':') {
                const auto slash = configuration.find_last_of(L"/\\");
                library = configuration.substr(0, slash + 1) + library;
            }
            SetEnvironmentVariableW(L"UCAL_VECTOR_XL_RUNTIME", library.c_str());
        }
    }
    return std::make_unique<Ucal::VectorBackend::Backend>();
}
