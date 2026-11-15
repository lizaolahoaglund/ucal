# PCAN-View support interface

`UcalPcanViewNativeBridge.exe` is a public thin launcher. It loads the adjacent
`UcalPcanViewSupport.dll` by absolute path and invokes `UcalPcanViewSupportRunV1`.
The [C declaration](../include/UcalPcanViewSupport.h) passes an argument count and
caller-owned UTF-16 strings, valid for the duration of the call. The support DLL
owns application supervision and cleanup and returns a process exit code. No
C++ exception, Qt object, STL object or memory ownership crosses this boundary.

The matching `UcalPcanViewNativeAdapter.dll` runs inside the selected application.
The two binaries share a compiled profile build identifier. A mismatch is rejected
before installing hooks. The binary support license is separate from the public
core license; see [license scope](../../../LICENSE_SCOPE.md).

Public source builds the Manager profile, thin launcher and original ABI fixture.
Running the installed integration requires the matching support and adapter binaries.

The [source-built support fixture](../test/software/README.md) checks UTF-16 arguments containing
spaces, missing support files, incompatible exports and propagation of a
nonzero exit code. With the actual support DLL present, it also probes invalid
input without launching vendor software.
