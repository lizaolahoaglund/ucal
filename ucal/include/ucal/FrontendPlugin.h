#ifndef UCAL_FRONTEND_PLUGIN_H
#define UCAL_FRONTEND_PLUGIN_H
#include <stdint.h>
#ifdef _WIN32
#define UCAL_FRONTEND_CALL __cdecl
#else
#define UCAL_FRONTEND_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define UCAL_FRONTEND_ABI_V1 0x00010000u
#define UCAL_FRONTEND_ABI_V2 0x00020000u
#define UCAL_FRONTEND_ABI_CURRENT UCAL_FRONTEND_ABI_V2
#define UCAL_FRONTEND_MAX_JSON (1024u * 1024u)
enum UcalFrontendOperation {
    UCAL_FRONTEND_SCAN = 1,
    UCAL_FRONTEND_INSPECT = 2,
    UCAL_FRONTEND_CHECK_COMPATIBILITY = 3,
    UCAL_FRONTEND_PREPARE_LAUNCH = 4,
    UCAL_FRONTEND_DEPLOY = 5,
    UCAL_FRONTEND_RESTORE_DEPLOYMENT = 6
};
/* V2 uses lower_snake_case JSON fields and operation names (vendor_code,
 * working_directory, prepare_launch). V1 preserves the original camelCase JSON.
 * The two versions share this C layout and the numeric operation IDs above.
 * Stateless, synchronous operations. UTF-8 JSON objects, caller-owned buffers;
 * never expose C++/Qt objects or exceptions. Export UcalFrontendGetApi with
 * its exact undecorated name. No output-size retry of a state-changing call.
 * Status: 0 success, 1 unsupported, 2 invalid request, 3 operation failed.
 * V2 failures may return {"error":"...", "vendor_code":123}; V1 uses vendorCode.
 * Host always supplies MAX_JSON output capacity before the call. Entry points
 * must reject insufficient output capacity before doing work. Do not retain
 * pointers or background work after invoke returns. No GUI in scan/inspect.
 */
typedef int32_t(UCAL_FRONTEND_CALL *UcalFrontendInvokeFunctionV1)(uint32_t operation, const char *request,
                                                          uint32_t requestBytes, char *output,
                                                          uint32_t outputCapacity, uint32_t *outputBytes);
typedef struct UcalFrontendApiV1 {
    uint32_t size;
    uint32_t abiVersion;
    uint32_t reserved;
    UcalFrontendInvokeFunctionV1 invoke;
} UcalFrontendApiV1;
/* V2 keeps the C layout/operation numbers and uses snake_case JSON field and
 * operation names. V1 retains its original camelCase JSON contract. Environment
 * variable names and other opaque identifiers are never case-normalized. */
typedef UcalFrontendApiV1 UcalFrontendApiV2;
typedef int32_t(UCAL_FRONTEND_CALL *UcalFrontendGetApiFunctionV1)(uint32_t abi, UcalFrontendApiV1 *api);
typedef UcalFrontendGetApiFunctionV1 UcalFrontendGetApiFunctionV2;
int32_t UCAL_FRONTEND_CALL UcalFrontendGetApi(uint32_t abi, UcalFrontendApiV1 *api);
#ifdef __cplusplus
}
#endif
#endif
