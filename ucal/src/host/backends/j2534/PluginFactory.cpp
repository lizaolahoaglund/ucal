#include "backends/j2534/J2534Backend.hpp"
std::unique_ptr<Ucal::Backend::BackendInterface> makeUcalPlugin(const std::wstring &path) {
    if (path.empty())
        return nullptr;
    return std::make_unique<Ucal::J2534Backend::Backend>(path);
}
