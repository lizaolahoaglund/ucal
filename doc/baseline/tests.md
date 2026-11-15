# UCAL test scope

- The [public build and regression suite](../../test/Running.md) covers Windows x86 and x64 native builds, the core protocol, backends, API frontends and UCAL Manager.
- The [Manager case inventory](../../manager/test/cases.json) identifies each case, its coverage area and its assertions. Qt reports results by architecture and counts setup and cleanup separately from functional cases.
- The [package tests](../../manager/doc/PackageLifecycle.md#build-and-validate) check package contents, runtime dependencies, and startup and exit from a freshly extracted directory.
- The [installed CANoe suite](../../frontends/canoe/test/software/README.md) selects CANoe 11 or 12 cases according to the installed versions and reports absent versions. The PCAN-View public suite includes [support DLL contract tests](../../frontends/pcanview/test/software/README.md).
- [Hardware tests](../../ucal/test/hardware/README.md) require the matching adapters, installed drivers and an independent wired peer. See the [support matrix](../support.md) for the versions and pairings tested.

Separate cases cover first startup, dialogs, abnormal exits, normal shutdown and resource release. Reports include failures, skips and their reasons. Hardware runs report their results separately.
