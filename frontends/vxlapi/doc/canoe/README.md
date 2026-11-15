# CANoe RuntimeKernel compatibility profile

UCAL Manager can launch a supported CANoe installation with its application-local
RuntimeKernel/XL profile. It prepares a local copy and selects the mapped CAN
channels for that run. Install CANoe and the Vector runtime separately and use
the matching UCAL compatibility support binaries.

Follow the [CANoe launch guide](../../../canoe/doc/Launch.md) to select an
installation, assign channels, choose a configuration and launch it from Manager.
The [support matrix](../../../../doc/support.md) lists the tested versions,
traffic modes and bench scope.

The Vector frontend accepts Classic CAN and CAN FD configuration through XL and
uses the selected backend's timing capabilities. See the
[Vector frontend](../README.md) and [CAN timing contract](../../../../ucal/doc/CanTiming.md).
The separately named Vector Provider profile remains a qualification candidate.
