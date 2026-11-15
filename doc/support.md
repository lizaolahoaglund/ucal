# Application and backend support scope

UCAL requires ISO CAN FD-capable CAN endpoints. Compatible endpoints can run
Classic CAN and ISO CAN FD sessions according to the application and driver.

| Component | Implemented / verified scope | Remaining qualification |
| --- | --- | --- |
| VXLAPI frontend | Independent x86/x64 public XL DLLs; static-import application-local deployment/restore; deterministic Classic/FD+BRS routing through original native/J2534 test runtimes | Installed SDK demos and individual programs require their own loading and API-subset acceptance |
| PCAN-Basic frontend | Independent x86/x64 PCANBasic.dll; stable process-lifetime channel handles; static-import deployment/restore; native-backend Classic/FD+BRS and J2534 Classic routing | Precise FD timing requires an advanced-timing backend; managed/dynamic loaders and individual SDK demos need separate qualification |
| CANoe | Application-local RuntimeKernel/XL profile. CANoe 11.0.42.0 x64 Manager mapping/launch, Classic 500k and ISO FD+BRS 500k/2M, 16 frames/direction with two independent PCAN-USB Pro FD adapters. CANoe 12.0.75.0 x64 Manager mapping and launch through standard and deep local-data paths, Classic 500k and ISO FD+BRS 500k/2M, 16 frames/direction on PCAN-USB FD and an independent VN1610 | Other installations, broader channels, load/fault behavior and the separately named Provider registration contract require qualification |
| PCAN-View | x64 5.0.2.838 and 6.0.0.1040 executable comparator; capability-gated precise timing or bitrate/sample-point targets, native configuration and bounded CAN/FD acceptance | Version/hash-pinned engineering profile; not a general supported PEAK plugin contract |
| PCAN-Basic backend | Deterministic x86/x64, bounded two-channel timing/traffic/lifecycle tests, and two independent PCAN-USB Pro FD CAN1-to-CAN1 Classic/FD acceptance | Physical hot-plug, injected passive/bus-off and cross-adapter fault-containment gates remain |
| Vector XL backend | Deterministic x86/x64 physical/virtual port identity across XL index/name changes, official virtual runtime, bounded VN1610 CAN 1 Classic/ISO FD+BRS against an independent peer | Physical hot-plug, electrical listen-only, injected faults and broader device coverage remain |
| J2534 | Standard 04.04 + GMW17753 raw CAN FD adapter; separate-Open and shared-DeviceID/Connect models, deterministic multichannel/discovery tests | Every vendor runtime requires its own bounded acceptance; Vector static interface evidence does not establish simultaneous multiport GM FD |

PCAN-Basic writes prove queue acceptance; they do not provide transaction-correlated physical completion. Unavailable error counters/readback remain invalid or unsupported. J2534 does not supply universal physical-port enumeration; an adapter uses a documented vendor discovery contract, or truthfully exposes only a supported default route.

API DLLs translate into the same UCAL device model for every backend. This composition does not make an unsupported parameter available: PCAN-Basic FD initialization supplies precise nominal timing and optional data timing, while generic J2534 has no universal TSEG/SJW setter. Such a request returns an error unless the backend advertises and implements the advanced-timing contract. Native PCAN-Basic and Vector XL implement that contract within their controller/API limits. VXL can use a rate-only FD configuration when the application does not request a timing tuple. See [timing](../ucal/doc/CanTiming.md) and [deployment](api-frontends.md).

The CANoe RuntimeKernel profile selects precise timing for advanced-capable
endpoints. Its explicitly labelled target mode supports bitrate and whole-percent
sample points on other endpoints, including FD arbitration/data phases.
Application controller-timing inputs are rounded to the nearest whole-percent
target, including 87.5% to 88%. The independently loaded public XL API
preserves precise requests and reports unsupported when they cannot be applied.
CANoe 11 Setup owns explicit sampling and timing. UCAL preserves those inputs
through capability-based translation. A request containing only bitrate prefers
75%. PCAN-Basic Classic computes that timing when available and otherwise selects
its closest valid timing. The documented CANoe 11 and 12 configurations
include normal stop and release of the mapped channels.

Manager starts its managed CANoe copy with the compatibility DLLs from the
selected package. The first application launch prepares that copy from the
user's installed CANoe and Vector runtime; the UCAL ZIP contains only original
UCAL integration modules, not vendor executables or libraries. Compatibility
inspection checks prerequisites without preparing or changing the copy.
Keep CANoe closed while these DLLs are updated; an in-use copy
blocks the update and launch. Existing forwarding shells and saved application
configuration are preserved.

The build and fixture tests run without vendor software or hardware. To verify an
installed application or physical link, follow the [application regression guide](../test/Running.md)
or [hardware procedures](../ucal/test/hardware/README.md). The
[PCAN backend](../backends/pcan_basic/doc/Backend.md) and
[Vector backend](../backends/vxlapi/doc/Backend.md) describe their tested hardware and timing.

The PCAN-View 6.0.0.1040 Manager path has bounded launch, exact endpoint-open and
clean-close checks with Vector Virtual CAN 1 and VN1610 CAN 1. Frame delivery,
load and fault qualification follow the separately documented backend evidence.

Installed CANoe and PCAN-View integrations require the matching compatibility
support binaries. See the [package guide](../manager/doc/PackageLifecycle.md#build-and-validate)
for the runtime files and [license scope](../LICENSE_SCOPE.md) for distribution terms.
