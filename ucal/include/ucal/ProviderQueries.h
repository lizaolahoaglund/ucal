#ifndef UCAL_PROVIDER_QUERIES_H
#define UCAL_PROVIDER_QUERIES_H
#include <stdint.h>
#ifdef _WIN32
#define UCAL_PROVIDER_CALL __stdcall
#else
#define UCAL_PROVIDER_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Optional provider-specific records, outside SAE J2534 and GMW17753. The
 * concrete package explicitly supplies export names. No vendor-name probing.
 * Caller sets size/version (1), owns the buffers, and validates the response. */
#pragma pack(push, 1)
typedef struct UcalProviderCanTimingV1 {
    uint32_t size, version, nominalBitrate, dataBitrate, nominalSample, dataSample;
} UcalProviderCanTimingV1;
typedef struct UcalProviderPortV1 {
    uint32_t size, version;
    char selector[48];
    uint32_t physicalPort, capabilities;
} UcalProviderPortV1;
#pragma pack(pop)
typedef int32_t(UCAL_PROVIDER_CALL *UcalProviderReadCanTimingFunctionV1)(uint32_t channel, UcalProviderCanTimingV1* timing);
typedef int32_t(UCAL_PROVIDER_CALL *UcalProviderEnumeratePortsFunctionV1)(uint32_t size, uint32_t version,
    UcalProviderPortV1* ports, uint32_t* count);
#ifdef __cplusplus
}
#endif
#endif
