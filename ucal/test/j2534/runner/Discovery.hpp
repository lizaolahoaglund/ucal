#pragma once
#include "ucal/J2534Discovery.h"
void runPublicDiscovery(const std::string &scenario) {
    wchar_t config[2048]{};
    GetEnvironmentVariableW(L"UCAL_J2534_CONFIG", config, 2048);
    wchar_t runtime[2048]{}, helper[2048]{}, native[2048]{};
    GetPrivateProfileStringW(L"provider", L"runtime", L"", runtime, 2048, config);
    GetPrivateProfileStringW(L"provider", L"discovery_library", L"", helper, 2048, config);
    GetPrivateProfileStringW(L"provider", L"test_native", L"", native, 2048, config);
    const auto sdk =
        LoadLibraryExW(native, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto module =
        LoadLibraryExW(helper, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(sdk && module, "public discovery libraries");
    const auto discover =
        reinterpret_cast<UcalJ2534DiscoverFunctionV2>(GetProcAddress(module, "UcalJ2534DiscoverV2"));
    const auto release = reinterpret_cast<UcalJ2534DiscoveryReleaseFunctionV2>(
        GetProcAddress(module, "UcalJ2534DiscoveryReleaseV2"));
    using Counts = void(__cdecl *)(uint32_t *, uint32_t *);
    const auto counts = reinterpret_cast<Counts>(GetProcAddress(sdk, "UcalTestDiscoveryCounts"));
    require(discover && release && counts, "public C discovery contract");
    std::array<UcalJ2534PortV2, 64> ports{};
    uint32_t count{};
    auto status = discover(reinterpret_cast<const uint16_t *>(runtime), ports.data(), 64, &count);
    if (scenario.find("oversized") != std::string::npos)
        require(status == static_cast<int32_t>(Status::TransportError),
                "unbounded native inventory accepted");
    else if (scenario.find("no-identity") != std::string::npos ||
             scenario.find("duplicate-identity") != std::string::npos)
        require(status == static_cast<int32_t>(Status::Unsupported) && count == 0u,
                "unidentifiable or ambiguous port became assignable");
    else {
        const bool shared = scenario.find("shared") != std::string::npos;
        require(status == 0 && count == (shared ? 1u : 2u), "public inventory count/FD filter");
        if (scenario.find("full-buffer") != std::string::npos)
            require(ports[1].port.physicalPort == 65, "native inventory truncated before FD filtering");
        const auto identity = std::string(ports[0].port.identity);
        for (unsigned i = 0; i < 30; ++i) {
            require(discover(reinterpret_cast<const uint16_t *>(runtime), ports.data(), 64, &count) == 0 &&
                        std::string(ports[0].port.identity) == identity,
                    "refresh identity or discovery lifecycle");
        }
        if (shared) {
            require(ports[0].connectionModel == UCAL_J2534_OPEN_SHARED, "shared device model not preserved");
            SetEnvironmentVariableA("UCAL_DISCOVERY_TEST_MODE", "mapping-changed");
            require(discover(reinterpret_cast<const uint16_t *>(runtime), ports.data(), 64, &count) == 0 &&
                        count == 1 && ports[0].port.physicalPort == 1,
                    "public mapping not reread");
            SetEnvironmentVariableA("UCAL_DISCOVERY_TEST_MODE", "unmapped");
            require(discover(reinterpret_cast<const uint16_t *>(runtime), ports.data(), 64, &count) == 0 &&
                        count == 0,
                    "unmapped provider created fake endpoints");
        } else
            require(ports[0].connectionModel == UCAL_J2534_OPEN_PER_PORT, "named port contract");
    }
    uint32_t init{}, closed{};
    counts(&init, &closed);
    const bool noInit = scenario.find("no-init") != std::string::npos;
    require(init == (noInit ? 0u : 1u) && closed == 0, "SDK reinitialized or released during refresh");
    release();
    counts(&init, &closed);
    require(closed == (noInit ? 0u : 1u), "SDK not released exactly once");
    FreeLibrary(module);
    FreeLibrary(sdk);
}
