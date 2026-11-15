#include "ucal/BackendPlugin.h"
#include "ucal/FrontendPlugin.h"
#include "ucal/J2534Discovery.h"
#include "ucal/ProviderQueries.h"
#include <stddef.h>
#define UCAL_STATIC_ASSERT(condition, name) typedef char name[(condition) ? 1 : -1]
UCAL_STATIC_ASSERT(UCAL_BACKEND_ABI_V1 == 0x00010000u, Backend_ABI_major);
UCAL_STATIC_ASSERT(UCAL_FRONTEND_ABI_V1 == 0x00010000u, Frontend_ABI_major);
UCAL_STATIC_ASSERT(UCAL_FRONTEND_CHECK_COMPATIBILITY == 3, Operation_numbers_are_stable);
UCAL_STATIC_ASSERT(offsetof(UcalBackendApiV1, create) == 24, Backend_prefix);
UCAL_STATIC_ASSERT(sizeof(UcalBackendApiV1) == 24 + 3 * sizeof(void*), Backend_layout);
UCAL_STATIC_ASSERT(sizeof(UcalFrontendApiV1) == (sizeof(void*) == 8 ? 24 : 16), Frontend_layout);
UCAL_STATIC_ASSERT(sizeof(UcalJ2534PortV1) == 1176, Discovery_V1_layout);
UCAL_STATIC_ASSERT(sizeof(UcalJ2534PortV2) == 1336, Discovery_V2_layout);
UCAL_STATIC_ASSERT(sizeof(UcalProviderCanTimingV1) == 24, Optional_query_timing_layout);
UCAL_STATIC_ASSERT(sizeof(UcalProviderPortV1) == 64, Optional_query_port_layout);
UCAL_STATIC_ASSERT(sizeof(UcalFrontendApiV2) == sizeof(UcalFrontendApiV1), Frontend_V2_same_C_layout);
int main(void) { return 0; }
