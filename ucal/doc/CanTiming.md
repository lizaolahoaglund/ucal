# Precise CAN timing

`CapabilityCanAdvancedTiming` identifies a backend that accepts explicit CAN timing
through protocol 1.11. It does not promise that every bitrate or timing tuple fits
every controller. Native PCAN-Basic and Vector XL implement this capability;
backends without it return `Unsupported` for an advanced open.

`CanChannelConfig` has independent `nominalTimingSpecified` and
`dataTimingSpecified` flags. Each specified `CanBitTiming` contains:

| Field | Meaning |
| --- | --- |
| `clockHz`, `prescaler` | Reference clock and divider; both zero when only bitrate and time-quanta ratios are known |
| `tseg1`, `tseg2` | Time quanta after synchronization, before and after the sample point |
| `sjw` | Synchronization jump width in time quanta; from 1 through TSEG2 |
| `sampleCount` | 1, or 3 for Classic CAN where supported; CAN FD requires 1 |

Bitrate remains explicit. With a reference clock, it must equal
`clockHz / (prescaler * (1 + tseg1 + tseg2))` exactly. Without one, bitrate and
the tuple determine the physical sample point and SJW duration. TSEG1 and TSEG2
are positive and at most 65535 in the neutral contract; each backend imposes its
own API limits. Data timing requires CAN FD.

For example, 80 MHz, divider 1, TSEG1 139, TSEG2 20, SJW 8 means 500 kbit/s,
an 87.5% sample point and a 100 ns SJW. A backend may choose an equivalent clock
and divider only if both the sample point and SJW duration remain exact.
Unsupported combinations fail rather than silently selecting SJW 1 or rounding
the sample point. The legacy integer sample-point fields describe rate/sample
requests when no precise tuple is specified; a precise tuple is authoritative
for its phase.

## Serialized compatibility

Operation 25, `OpenCanChannelAdvanced`, requires minor version 11. Its 120-byte
`OpenCanChannelRequestV11` consists of the unchanged 60-byte V5 request, timing
flags, two 24-byte timing records and eight reserved zero bytes. Bit 0 selects
nominal timing and bit 1 selects data timing. At least one bit must be set;
unselected records and reserved bytes must be zero. Invalid records fail before
backend open.

Operation 26, `EnumerateEndpointsV11`, uses the V10 endpoint layout with the
advanced-timing capability bit. Older enumeration operations omit that bit.
Read-only enumeration can fall back to an older catalog. An advanced open has no
fallback that removes the requested parameters. Existing open layouts and
rate-only requests retain their original encoding.

Deterministic x86/x64 tests exercise exact nominal/data delivery through both
native hardware adapters, invalid SJW rejection and malformed serialized requests.
Physical qualification remains bounded to the combinations listed in each
backend's documentation; these fixtures do not expand hardware acceptance.
