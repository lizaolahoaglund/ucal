#include "ucal/Model.hpp"

#include <iomanip>
#include <sstream>

namespace Ucal {
namespace {

constexpr std::uint32_t discoveryCapabilities =
    CapabilityClassicCan | CapabilityCanFdIso | CapabilityBrs | CapabilityDiscoveryOnly;

const std::vector<DeviceDescriptor> devices{
    {{0x8d, 0x62, 0x2f, 0x5a, 0x77, 0x39, 0x4b, 0x61, 0xa4, 0x55, 0x11, 0x20, 0x00, 0x00, 0x00, 0x02},
     "Unified CAN Mock X2", "UCAL-MOCK-X2", "UCAL-X2-0001", 2, discoveryCapabilities,
     DeviceState::Available},
    {{0x8d, 0x62, 0x2f, 0x5a, 0x77, 0x39, 0x4b, 0x61, 0xa4, 0x55, 0x11, 0x20, 0x00, 0x00, 0x00, 0x04},
     "Unified CAN Mock X4", "UCAL-MOCK-X4", "UCAL-X4-0001", 4, discoveryCapabilities,
     DeviceState::Available},
};

}  // namespace

const std::vector<DeviceDescriptor>& mockCatalog() {
    return devices;
}

std::string deviceIdToString(const DeviceId& id) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < id.size(); ++index) {
        out << std::setw(2) << static_cast<unsigned>(id[index]);
        if (index == 3 || index == 5 || index == 7 || index == 9) {
            out << '-';
        }
    }
    return out.str();
}

}  // namespace ucal
