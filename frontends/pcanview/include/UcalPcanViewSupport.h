#ifndef UCAL_PCANVIEW_SUPPORT_H
#define UCAL_PCANVIEW_SUPPORT_H

#include <stdint.h>

#if defined(_WIN32)
#define UCAL_PCANVIEW_SUPPORT_CALL __cdecl
#else
#define UCAL_PCANVIEW_SUPPORT_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Arguments are caller-owned UTF-16 strings, valid for the duration of the call.
 * The support module owns supervision and cleanup and returns a process exit code.
 * No memory ownership or C++/Qt object crosses this interface. */
typedef int32_t (UCAL_PCANVIEW_SUPPORT_CALL *UcalPcanViewSupportRunFunctionV1)(
    uint32_t argc, const uint16_t* const* argv);

#ifdef __cplusplus
}
#endif
#endif
