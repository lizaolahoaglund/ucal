#pragma once

#include "ucal/Model.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Ucal {

// Vector exposes one 64-bit channel mask across the catalog. Keep all CAN
// endpoints ahead of LIN endpoints so a mixed adapter inventory cannot consume
// or shift CAN slots. The remaining keys make the mapping independent of
// backend enumeration order and stable for the same set of physical adapters.
inline bool normalizeVectorFrontendCatalog(
    std::vector<EndpointDescriptor>& endpoints, const std::string& selectedSerial = {},
    const std::string& selectedCanChannels = {},
    const std::string& selectedCanEndpointIds = {},
    const bool canOnly = false) {
    const auto selectorCount = static_cast<unsigned int>(!selectedSerial.empty()) +
                                static_cast<unsigned int>(!selectedCanChannels.empty()) +
                                static_cast<unsigned int>(!selectedCanEndpointIds.empty());
    if (selectorCount > 1u) {
        endpoints.clear();
        return false;
    }
    // A CAN-only application profile must not advertise unrelated LIN channels
    // to a consumer whose LIN discovery contract has not been accepted.
    if (canOnly) {
        endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
            return endpoint.busType != BusType::Can;
        }), endpoints.end());
    }
    if (!selectedSerial.empty()) {
        endpoints.erase(
            std::remove_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
                return endpoint.serial != selectedSerial;
            }),
            endpoints.end());
    }

    std::sort(endpoints.begin(), endpoints.end(), [](const auto& left, const auto& right) {
        const auto leftBus = left.busType == BusType::Can ? 0u : 1u;
        const auto rightBus = right.busType == BusType::Can ? 0u : 1u;
        if (leftBus != rightBus) return leftBus < rightBus;
        if (left.adapterId != right.adapterId) return left.adapterId < right.adapterId;
        if (left.physicalIndex != right.physicalIndex) {
            return left.physicalIndex < right.physicalIndex;
        }
        return left.id < right.id;
    });

    if (selectedCanChannels.empty() && selectedCanEndpointIds.empty()) return true;

    std::vector<EndpointDescriptor> selected;
    if (!selectedCanEndpointIds.empty()) {
        std::size_t begin = 0u;
        while (begin < selectedCanEndpointIds.size()) {
            const auto end = selectedCanEndpointIds.find(';', begin);
            auto token = selectedCanEndpointIds.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            if (token.size() != 36u) {
                endpoints.clear();
                return false;
            }
            for (std::size_t index = 0u; index < token.size(); ++index) {
                const auto character = static_cast<unsigned char>(token[index]);
                const bool separator = index == 8u || index == 13u || index == 18u ||
                                       index == 23u;
                if ((separator && character != '-') ||
                    (!separator && !std::isxdigit(character))) {
                    endpoints.clear();
                    return false;
                }
                token[index] = static_cast<char>(std::tolower(character));
            }
            const auto matching = [&](const auto& endpoint) {
                return endpoint.busType == BusType::Can &&
                       deviceIdToString(endpoint.id) == token;
            };
            const auto first = std::find_if(endpoints.begin(), endpoints.end(), matching);
            if (first == endpoints.end() ||
                std::find_if(std::next(first), endpoints.end(), matching) != endpoints.end() ||
                std::any_of(selected.begin(), selected.end(), [&](const auto& endpoint) {
                    return endpoint.id == first->id;
                })) {
                endpoints.clear();
                return false;
            }
            selected.push_back(*first);
            if (end == std::string::npos) break;
            begin = end + 1u;
            if (begin == selectedCanEndpointIds.size()) {
                endpoints.clear();
                return false;
            }
        }
        std::copy_if(endpoints.begin(), endpoints.end(), std::back_inserter(selected),
                     [](const auto& endpoint) { return endpoint.busType == BusType::Lin; });
        endpoints = std::move(selected);
        return true;
    }

    std::size_t begin = 0u;
    while (begin < selectedCanChannels.size()) {
        const auto end = selectedCanChannels.find(';', begin);
        const auto token = selectedCanChannels.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto separator = token.rfind('|');
        if (separator == std::string::npos || separator == 0u ||
            separator + 1u >= token.size()) {
            endpoints.clear();
            return false;
        }
        const auto serial = token.substr(0u, separator);
        const auto portText = token.substr(separator + 1u);
        std::uint64_t port = 0u;
        for (const auto character : portText) {
            if (!std::isdigit(static_cast<unsigned char>(character))) {
                endpoints.clear();
                return false;
            }
            port = port * 10u + static_cast<unsigned int>(character - '0');
            if (port > (std::numeric_limits<std::uint32_t>::max)()) {
                endpoints.clear();
                return false;
            }
        }
        const auto matching = [&](const auto& endpoint) {
            return endpoint.busType == BusType::Can && endpoint.serial == serial &&
                   endpoint.physicalIndex == port;
        };
        const auto first = std::find_if(endpoints.begin(), endpoints.end(), matching);
        if (first == endpoints.end() ||
            std::find_if(std::next(first), endpoints.end(), matching) != endpoints.end() ||
            std::any_of(selected.begin(), selected.end(), [&](const auto& endpoint) {
                return endpoint.id == first->id;
            })) {
            endpoints.clear();
            return false;
        }
        selected.push_back(*first);
        if (end == std::string::npos) break;
        begin = end + 1u;
        if (begin == selectedCanChannels.size()) {
            endpoints.clear();
            return false;
        }
    }
    std::copy_if(endpoints.begin(), endpoints.end(), std::back_inserter(selected),
                 [](const auto& endpoint) { return endpoint.busType == BusType::Lin; });
    endpoints = std::move(selected);
    return true;
}

}  // namespace ucal
