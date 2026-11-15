#ifndef UCAL_J2534_DISCOVERY_H
#define UCAL_J2534_DISCOVERY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional UCAL package extension, NOT a J2534 export. The original discovery
 * DLL belongs to the concrete package; the unmodified vendor PassThru DLL is
 * still used for all device access. No bus handles are opened or device configuration changed here.
 * One serialized call, caller-owned records, bounded UTF-8 strings, zero reserved.
 * Returns UCAL Status; catches exceptions. runtime_path is UTF-16 on Windows.
 * capacity is at most 64; excess inventory returns invalid_argument, not truncation.
 */
#define UCAL_J2534_DISCOVERY_V1 1u
#define UCAL_J2534_DISCOVERY_FD 1u
#define UCAL_J2534_DISCOVERY_BRS 2u
#define UCAL_J2534_DISCOVERY_TERMINATION 4u
typedef struct UcalJ2534PortV1 {
    uint32_t size, version, physicalPort, capabilities;
    char identity[128], adapterIdentity[128], selector[513];
    char name[128], model[128], serial[128];
    uint32_t reserved;
} UcalJ2534PortV1;
#if defined(_WIN32)
#define UCAL_DISCOVERY_CALL __cdecl
#else
#define UCAL_DISCOVERY_CALL
#endif
typedef int32_t (UCAL_DISCOVERY_CALL *UcalJ2534DiscoverFunctionV1)(
    const uint16_t *runtimePath, UcalJ2534PortV1 *ports, uint32_t capacity, uint32_t *count);

/* Version 2 adds routing, without adding a vendor-specific transport API.
 * port.size is sizeof(UcalJ2534PortV2), port.version is 2. Strings remain bounded
 * UTF-8 and all reserved fields must be zero. Named ports acquire separate
 * DeviceIDs. Shared ports with the same device_key reuse one Open/DeviceID;
 * their protocol/pin routes must be distinct. fd_protocol is the published GM
 * raw FD protocol, never inferred from classic_protocol or native capabilities.
 * pin_assignment==0 inherits the provider's assignment. device_key is required
 * only for shared Open. The module stays loaded until all sessions are closed.
 */
#define UCAL_J2534_DISCOVERY_V2 2u
#define UCAL_J2534_OPEN_PER_PORT 1u
#define UCAL_J2534_OPEN_SHARED 2u
typedef struct UcalJ2534PortV2 {
    UcalJ2534PortV1 port;
    uint32_t connectionModel, classicProtocol, fdProtocol, pinAssignment;
    char deviceKey[128];
    uint32_t reserved[4];
} UcalJ2534PortV2;
typedef int32_t(UCAL_DISCOVERY_CALL *UcalJ2534DiscoverFunctionV2)(const uint16_t *runtimePath,
                                                          UcalJ2534PortV2 *ports, uint32_t capacity,
                                                          uint32_t *count);
/* Optional explicit shutdown, called before unloading, outside loader lock. */
typedef void(UCAL_DISCOVERY_CALL *UcalJ2534DiscoveryReleaseFunctionV2)(void);
int32_t UCAL_DISCOVERY_CALL UcalJ2534DiscoverV1(const uint16_t *runtimePath,
    UcalJ2534PortV1 *ports, uint32_t capacity, uint32_t *count);
int32_t UCAL_DISCOVERY_CALL UcalJ2534DiscoverV2(const uint16_t *runtimePath,
    UcalJ2534PortV2 *ports, uint32_t capacity, uint32_t *count);
void UCAL_DISCOVERY_CALL UcalJ2534DiscoveryReleaseV2(void);
#ifdef __cplusplus
}
#endif
#endif
