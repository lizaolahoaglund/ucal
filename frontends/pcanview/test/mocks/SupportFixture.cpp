#include "../../include/UcalPcanViewSupport.h"

namespace {
bool equal(const uint16_t* actual, const char16_t* expected) {
    if (!actual) return false;
    for (unsigned index = 0; index < 64u; ++index) {
        if (actual[index] != static_cast<uint16_t>(expected[index])) return false;
        if (!expected[index]) return true;
    }
    return false;
}
}

extern "C" __declspec(dllexport) int32_t UCAL_PCANVIEW_SUPPORT_CALL UcalPcanViewSupportRunV1(
    uint32_t argc, const uint16_t* const* argv) {
    if (argc == 2u && argv && argv[0] && equal(argv[1], u"--exit-17")) return 17;
    return argc == 3u && argv && argv[0] &&
        equal(argv[1], u"\u53c2\u6570 \u901a\u9053") && equal(argv[2], u"--fixture") ? 0 : 2;
}
