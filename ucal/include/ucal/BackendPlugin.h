#ifndef UCAL_BACKEND_PLUGIN_H
#define UCAL_BACKEND_PLUGIN_H

#include <stdint.h>

#if defined(_WIN32)
#define UCAL_BACKEND_CALL __cdecl
#else
#define UCAL_BACKEND_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UCAL_BACKEND_ABI_V1 0x00010000u
#define UCAL_BACKEND_MAX_PAYLOAD 65536u

/* Original UCAL C ABI. Buffers are caller-owned; no C++ object crosses the DLL.
 * configuration is a bounded UTF-8 absolute configuration-file path (or empty).
 * invoke uses UCAL little-endian protocol operation/payload definitions. The host
 * supplies MAX_PAYLOAD output space before invoking any state-changing operation;
 * a short buffer is rejected without consuming data or executing the operation.
 * Calls for one instance are serialized by the host. create/destroy delimit all
 * resources. A plugin must catch its exceptions before returning an int32 status.
 */
typedef int32_t(UCAL_BACKEND_CALL *UcalBackendCreateFunctionV1)(const char *configuration,
                                                        uint32_t configurationBytes, void **instance);
typedef void(UCAL_BACKEND_CALL *UcalBackendDestroyFunctionV1)(void *instance);
typedef int32_t(UCAL_BACKEND_CALL *UcalBackendInvokeFunctionV1)(void *instance, uint32_t ownerPid,
                                                        uint16_t operation, uint16_t protocolMinor,
                                                        const void *input, uint32_t inputBytes, void *output,
                                                        uint32_t outputCapacity, uint32_t *outputBytes);

typedef struct UcalBackendApiV1 {
    uint32_t size;
    uint32_t abiVersion;
    uint32_t protocolMajor;
    uint32_t protocolMinor;
    uint32_t maxPayloadBytes;
    uint32_t reserved;
    UcalBackendCreateFunctionV1 create;
    UcalBackendDestroyFunctionV1 destroy;
    UcalBackendInvokeFunctionV1 invoke;
} UcalBackendApiV1;

/* Export exactly this undecorated name using the supplied .def example. */
typedef int32_t(UCAL_BACKEND_CALL *UcalBackendGetApiFunctionV1)(uint32_t requestedAbi, UcalBackendApiV1 *api);
int32_t UCAL_BACKEND_CALL UcalBackendGetApi(uint32_t requestedAbi, UcalBackendApiV1 *api);

#ifdef __cplusplus
}
#endif
#endif
