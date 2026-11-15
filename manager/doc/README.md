# UCAL Manager

UCAL Manager is the Qt 5.14.2 control-plane application. The executable is `UcalManager.exe`; code uses `Ucal::Manager`.

It discovers installed software through software plugins, scans concrete backend packages, displays their live hardware endpoints and saves per-application logical-channel mappings. Applications, installed software instances, plugin metadata and live hardware state are separate concepts.

Use the sidebar in this order: Applications, Software, Drivers, Hardware. The Software page scans supported installations or accepts an EXE for inspection. The Drivers page enables/disables a concrete backend, rescans packages and opens a declared vendor configuration tool. The Hardware page supports a temporary keyword filter and Windows USB refresh. Applications select their software instance and map unique logical channels; automatic mode selects the first compatible endpoints up to that software's declared limit.

Parameters configurable in the external application remain application-owned. Manager provides an editor only where the profile declares a needed backend-supported fallback. No competing saved bitrate/sample-point default overrides the application's supported editor.

Portable user data and diagnostics live under the program's `data` directory. Metadata JSON contains static package properties, not discovered channels or online status. The public UI uses **UCAL Manager**, while code types and original binary names use **UcalManager**. Persisted identity strings retain their existing spelling.

Build: `UcalManager.pro`. Shared software-plugin C entry code: `frontend`. Generic control-plane models: `control`. Private headers stay beside implementation. See [layout](../../doc/layout.md), [user workflow](j2534-user-guide.md), [packages](plugin-packages.md), [CI](../../test/Running.md) and [support scope](../../doc/support.md).
