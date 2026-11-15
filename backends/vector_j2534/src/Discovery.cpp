#include "NativeDiscoveryAbi.hpp"
#include "ucal/J2534DiscoveryHelpers.hpp"

namespace {
using namespace Ucal;
using namespace Ucal::J2534Discovery;
using Config = NativeDiscoveryAbi::XlDriverConfig;
using Driver = int16_t(__stdcall *)();
using GetConfig = int16_t(__stdcall *)(Config *);
using GetApplication = int16_t(__stdcall *)(char *, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t);
Library library;
Driver closeDriver{};
bool initialized{};
} // namespace
extern "C" void __cdecl UcalJ2534DiscoveryReleaseV2() {
    if (initialized && closeDriver)
        closeDriver();
    initialized = false;
    closeDriver = nullptr;
    library.release();
}
extern "C" int32_t __cdecl UcalJ2534DiscoverV2(const uint16_t *runtime, UcalJ2534PortV2 *output,
                                                  uint32_t capacity, uint32_t *count) {
    if (!runtime || !output || !count || capacity > 64)
        return code(Status::InvalidArgument);
    *count = 0;
    try {
#if defined(_WIN64)
        const auto companion = L"vxlapi64.dll";
#else
        const auto companion = L"vxlapi.dll";
#endif
        if (!library.load(runtime, companion, true))
            return code(Status::NotFound);
        const auto openDriver = library.function<Driver>("xlOpenDriver");
        closeDriver = library.function<Driver>("xlCloseDriver");
        const auto query = library.function<GetConfig>("xlGetDriverConfig");
        const auto mapping = library.function<GetApplication>("xlGetApplConfig");
        if (!openDriver || !closeDriver || !query || !mapping)
            return code(Status::IncompatibleVersion);
        if (!initialized) {
            if (openDriver() != 0)
                return code(Status::NotFound);
            initialized = true;
        }
        Config config{};
        if (query(&config) != 0 || config.channelCount > 64)
            return code(Status::TransportError);
        uint32_t type{}, index{}, channel{};
        char app[] = "PassThruXLLibrary";
        if (mapping(app, 0, &type, &index, &channel, 1) != 0)
            return 0;
        std::vector<UcalJ2534PortV2> list;
        for (uint32_t i = 0; i < config.channelCount; ++i) {
            const auto &info = config.channel[i];
            if (info.hardwareType != type || info.hardwareIndex != index ||
                info.hardwareChannel != channel)
                continue;
            if (info.hardwareType == 1 || !info.serialNumber || !(info.channelCapabilities & 0x80000000u))
                return 0;
            const auto adapter = "xl:" + std::to_string(type) + ":" + std::to_string(info.serialNumber);
            auto p = port(channel);
            p.connectionModel = UCAL_J2534_OPEN_SHARED;
            // SP17 documents additional Classic protocols only. Expose just the
            // mapped default GM-FD route, never infer FD_CHn from CAN_CHn.
            const auto name = bounded(info.name, sizeof(info.name));
            if (name.empty() || !field(p.deviceKey, "PassThruXLLibrary") ||
                !field(p.port.identity, adapter) || !field(p.port.adapterIdentity, adapter) ||
                !field(p.port.serial, std::to_string(info.serialNumber)) || !field(p.port.model, name) ||
                !field(p.port.name, name + " CAN " + std::to_string(channel + 1u)))
                return code(Status::TransportError);
            list.push_back(p);
            break;
        }
        return deliver(list, output, capacity, count);
    } catch (...) {
        return code(Status::InternalError);
    }
}
