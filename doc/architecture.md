# Unified CAN Abstraction Layer architecture

UCAL has three logical data-path layers:

1. Consumer compatibility contracts translate external software calls into UCAL.
2. The neutral core and Broker own model, protocol, identity, routing, leases, lifecycle and truthful capabilities/events.
3. Hardware backends translate UCAL operations to authorized user-mode device APIs.

UCAL Manager is the control plane. Software installations and saved applications are data, not additional layers. Public XL consumers and CANoe use different loading profiles of the same Vector-facing contract. A hardware vendor can independently appear on either side. Frontend/backend composition is a Cartesian product: no pair-specific route is needed for two conforming components.

`frontends` contains independent external-API frontends and installed-software profiles. External software calls a frontend, which translates into UCAL. An API-only frontend need not provide Manager software scanning/launch operations; only packages with a software template produce discovered installation instances. XL, PCAN and other consumer contracts share this frontend role. Hardware backends translate in the opposite direction, from UCAL into vendor-driver operations. Native and J2534 hardware adapters are separate peer source groups under `backends`; a native XL implementation does not own the Vector J2534 implementation. The shared J2534 04.04 + GMW17753 adapter lives inside neutral UCAL. Vendor-specific channel discovery belongs to its concrete source group. Standard J2534 transport and optional discovery/query contracts remain distinct.

The C plugin ABI uses versioned tables, explicit record sizes, caller-owned buffers and serialized messages. No Qt/STL object or C++ exception crosses a DLL boundary. The host supports x86 vendor DLLs through an x86 worker behind the x64 Broker. This is an internal mechanism, not a user setup requirement.

Backend endpoint identity follows adapter/port identity rather than a catalog's
global channel position. The Vector backend applies this to both serial-identified
physical ports and runtime-local Virtual CAN ports, so physical-device discovery
and display-name changes do not replace a Virtual CAN mapping.

Public API source groups are `frontends/vxlapi` and `frontends/pcan_basic`. They own their native DLLs, ABI records, deployment metadata and tests independently of software discovery/launch groups. The Vector group also owns its CANoe loading shells, which compile the same XL data plane; the CANoe Manager plugin consumes those runtime assets. A generic PE deployment transaction and parser live under `ucal/tools`; API-specific DLL names and entry requirements belong in each frontend's static `deployment.json`. API-only packages have no `frontend.json` and do not create software installation rows.

Static JSON describes a plugin, its library, supported operations and optional tool. It never stores a discovered hardware inventory. Channel enumeration happens at runtime; a missing saved endpoint remains an explicit offline mapping. Refresh does not reorder or substitute saved logical slots.

Configuration ownership is per parameter and software profile. If the software provides a supported user editor, it owns the parameter. Manager hides or marks its own field read-only and injects no competing fallback. Otherwise a needed, declared backend capability can supply a fallback. SET and GET support are independent; neither query success nor a displayed default fabricates actual hardware readback or physical Tx completion.

Precise CAN timing is a neutral optional capability. Protocol 1.11 carries nominal/data TSEG1, TSEG2, SJW and sampling count, with an optional reference clock and prescaler, in a separate versioned open request. A frontend preserves requested physical timing through the core; a backend either applies it exactly or returns unsupported. A rate-only request continues to use the existing contract. Older catalogs omit the advanced-timing bit, and an advanced open is never retried with timing removed. See the [timing contract](../ucal/doc/CanTiming.md).

The PCAN-View engineering profile selects its editor mode from the endpoint's
capabilities before opening a lease. A target-only backend receives bitrate and
whole-percent sample-point targets; the dialog discloses that mode and disables
detailed controller editing. An advanced-timing backend receives the selected
precise tuples. This policy belongs to the frontend and applies to every backend
advertising the same capability set.

The CANoe RuntimeKernel profile has an explicit target mode for endpoints without
advanced timing. It converts the application's controller tuples to
bitrate/whole-percent sample-point targets before opening a lease and identifies
that mode in the mapped hardware panel. Fractional sample points use the nearest
whole-percent target, including 87.5% to 88%. Unsupported sampling counts and
targets outside the representable range are rejected. Precise-capable endpoints and ordinary public XL
callers retain their exact timing tuples. An unsupported advanced open is never
retried with its parameters removed.
CANoe 11 Setup supplies explicit Classic BTR and FD sampling/timing parameters.
These requests follow the same capability-based conversion policy. A request
containing only bitrate uses the existing 75% sample-point default. Explicit
parameters remain application-owned through frontend translation and backend
configuration.

CANoe compatibility inspection reads launch assets and first-use prerequisites
without changing them. The first actual launch prepares the complete owned copy
from the user's installed application and architecture-matched Vector runtimes.
For deep local data paths, Manager selects a shorter installation-specific copy
directory for CANoe 11 and 12. Provisioning, launch and module synchronization use the same location.
Original UCAL code emits export-forwarding metadata locally without a compiler
or linker. A staged directory is activated only after preparation succeeds;
an existing incomplete directory is retained separately. Vendor code remains a
local installed prerequisite, outside the release payload. Actual launch then
synchronizes the selected profile's packaged x86/x64 data, discovery and
bundled C++ runtime DLLs into the Manager-owned launch copy. A per-copy lock and
write-access preflight prevent replacing a DLL set in use. Previous bytes and
hashes are retained before atomic file replacement; an incomplete update blocks
launch and restores replaced files. Provider forwarding shells and application
configuration are outside this update.

Manager starts and probes the Broker on its discovery worker so the window can appear before inventory is available. Product compatibility checks use the software profile's read-only launch preparation to check required runtime assets. Deploy and restore actions appear only for profiles declaring those operations; application-local API deployment is also available through the independent frontend tools.

See [source layout](layout.md), [C contracts](../ucal/doc/README.md), [packages](../ucal/doc/backend-plugins.md), [support scope](support.md) and the [regression guide](../test/Running.md).

Public-source scope is independent of logical layer placement. The core, Manager,
public backends, interface declarations and public frontend shells use LGPL-3.0
with the independent-module permission. The shared Vector compatibility runtime
and CANoe loading profiles are distributed as binary support, separate from the
PCAN-View support/adapter family. The public source includes the ordinary XL
forwarding shell and its discovery sidecar. See [license scope](../LICENSE_SCOPE.md) and
[PCAN-View support ABI](../frontends/pcanview/doc/Support.md).

Runtime release layout is separate from source ownership. Original compatibility
assets have a single module runtime location, while source-only tools stay in Git.
Backend manifests are static; per-machine runtime overrides live under Manager's
user settings and application mappings remain profile-owned. See the
[user package contract](../manager/doc/PackageLifecycle.md).
