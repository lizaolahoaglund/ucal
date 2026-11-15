#include "ucal/J2534Discovery.h"
#include <windows.h>
#include <array>

int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 1;
    const auto module =
        LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto v2 =
        module ? reinterpret_cast<UcalJ2534DiscoverFunctionV2>(GetProcAddress(module, "UcalJ2534DiscoverV2"))
               : nullptr;
    const auto v1 =
        module ? reinterpret_cast<UcalJ2534DiscoverFunctionV1>(GetProcAddress(module, "UcalJ2534DiscoverV1"))
               : nullptr;
    const auto input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    unsigned char request{};
    DWORD done{};
    while (ReadFile(input, &request, 1, &done, nullptr) && done == 1 && request == 1) {
        std::array<UcalJ2534PortV2, 64> routes{};
        std::array<UcalJ2534PortV1, 64> ports{};
        std::array<uint32_t, 3> header{2, 0, v2 ? 2u : 1u};
        int32_t status = 2;
        try {
            if (v2)
                status = v2(reinterpret_cast<const uint16_t *>(argv[2]), routes.data(), 64, &header[1]);
            else if (v1)
                status = v1(reinterpret_cast<const uint16_t *>(argv[2]), ports.data(), 64, &header[1]);
        } catch (...) {
            status = 8;
        }
        header[0] = (status >= 0 && status <= 8) ? static_cast<uint32_t>(status) : 7u;
        if (header[1] > 64) {
            header[0] = 7;
            header[1] = 0;
        }
        if (!WriteFile(output, header.data(), sizeof(header), &done, nullptr) || done != sizeof(header))
            break;
        if (!header[0] && header[1]) {
            const auto data =
                v2 ? static_cast<const void *>(routes.data()) : static_cast<const void *>(ports.data());
            const auto size = header[1] * (v2 ? sizeof(routes[0]) : sizeof(ports[0]));
            if (!WriteFile(output, data, static_cast<DWORD>(size), &done, nullptr) || done != size)
                break;
        }
    }
    if (module) {
        const auto release = reinterpret_cast<UcalJ2534DiscoveryReleaseFunctionV2>(
            GetProcAddress(module, "UcalJ2534DiscoveryReleaseV2"));
        if (release) {
            try {
                release();
            } catch (...) {
            }
        }
        FreeLibrary(module);
    }
    return 0;
}
