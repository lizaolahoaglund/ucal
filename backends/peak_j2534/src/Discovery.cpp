#include "NativeDiscoveryAbi.hpp"
#include "ucal/J2534DiscoveryHelpers.hpp"
#include <set>
#include <sstream>

namespace {
using namespace Ucal;
using namespace Ucal::J2534Discovery;
using Info = NativeDiscoveryAbi::PcanChannelInformation;
using GetValue = uint32_t(__stdcall *)(uint16_t, uint8_t, void *, uint32_t);
Library library;
} // namespace
extern "C" void __cdecl UcalJ2534DiscoveryReleaseV2() { library.release(); }
extern "C" int32_t __cdecl UcalJ2534DiscoverV2(const uint16_t *runtime, UcalJ2534PortV2 *output,
                                                  uint32_t capacity, uint32_t *count) {
    if (!runtime || !output || !count || capacity > 64)
        return code(Status::InvalidArgument);
    *count = 0;
    try {
        if (!library.load(runtime, L"PCANBasic.dll", true))
            return code(Status::NotFound);
        const auto query = library.function<GetValue>("CAN_GetValue");
        if (!query)
            return code(Status::IncompatibleVersion);
        uint32_t size{};
        if (query(0, 0x2a, &size, sizeof(size)) != 0)
            return code(Status::TransportError);
        if (size > 4096)
            return code(Status::TransportError);
        std::vector<Info> infos(size);
        if (size && query(0, 0x2b, infos.data(), static_cast<uint32_t>(infos.size() * sizeof(Info))) != 0)
            return code(Status::TransportError);
        std::vector<UcalJ2534PortV2> list;
        std::set<std::string> identities;
        for (const auto &info : infos) {
            if (!(info.features & 1u))
                continue;
            char guid[128]{};
            std::string adapter;
            if (query(info.handle, 0x30, guid, sizeof(guid)) == 0)
                adapter = bounded(guid, sizeof(guid));
            if (adapter.empty()) {
                if (!info.deviceId)
                    return code(Status::Unsupported);
                adapter = "pcan:" + std::to_string(info.deviceType) + ":" + std::to_string(info.deviceId);
            }
            const auto identity = adapter + ":" + std::to_string(info.controllerNumber);
            if (!identities.insert(identity).second)
                return code(Status::Unsupported);
            auto p = port(info.controllerNumber);
            std::ostringstream selector;
            selector << "J2534-2:PEAK 0x" << std::hex << info.handle;
            const auto model = bounded(info.deviceName, sizeof(info.deviceName));
            if (model.empty() || !field(p.port.identity, identity) ||
                !field(p.port.adapterIdentity, adapter) || !field(p.port.selector, selector.str()) ||
                !field(p.port.name, model + " CAN " + std::to_string(info.controllerNumber + 1u)) ||
                !field(p.port.model, model))
                return code(Status::TransportError);
            list.push_back(p);
        }
        return deliver(list, output, capacity, count);
    } catch (...) {
        return code(Status::InternalError);
    }
}
