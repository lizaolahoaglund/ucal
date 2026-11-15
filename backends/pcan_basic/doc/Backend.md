# PCAN-Basic backend

## Scope

This backend makes PEAK CAN/CAN FD hardware available behind the neutral UCAL Broker.
It is separate from the PCAN-Basic API and PCAN-View frontends: PCAN-Basic is the hardware
backend here, while Vector XL or another UCAL frontend is the application-facing API.

The implementation dynamically loads an installed `PCANBasic.dll` and uses the public
PCAN-Basic API documented by PEAK. No PEAK header or runtime is redistributed. Primary
references are PEAK's [PCAN-Basic product page](https://www.peak-system.com/products/software/development-packages/pcan-basic/?L=1),
[API package/download page](https://www.peak-system.com/support/software-information/development-kits/pcan-basic/),
and [parameter documentation](https://www.peak-system.com/produktcd/Develop/PC%20interfaces/Windows/PCAN-Basic%20API/PCAN-Parameter_Documentation.pdf).

## Implemented mapping

| UCAL concept | PCAN-Basic mapping |
|---|---|
| enumerate | attached-channel count and channel information |
| stable adapter/endpoint identity | public device GUID plus controller index when available |
| classic timing | exact BTR0/BTR1 tuple, or solved bitrate/sample-point target |
| CAN FD timing | documented nominal/data timing string |
| precise timing capability | exact TSEG/SJW and sample count; equivalent clock conversion only when physical intervals are preserved |
| applied timing verification | exact public nominal readback; exact data readback for CAN FD |
| listen-only | `PCAN_LISTEN_ONLY`, set before initialization and read back |
| bus-off reset policy | `PCAN_BUSOFF_AUTORESET` explicitly disabled after initialization and read back |
| receive | classic or FD queue read with hardware timestamp |
| opt-in self receive | official PCAN echo frames, marked `frame_self_received` |
| transmit | classic or FD write; result means queue acceptance |
| bus state | public status bits for warning/passive/bus-off |
| acceptance filters | exact UCAL software predicates |
| runtime-thread ownership | every PCAN-Basic call serialized on one dedicated backend thread |
| cleanup | uninitialize on close, process death, or Broker exit |

## Deliberately unsupported or unavailable

- PCAN-Basic exposes no programmable 120-ohm termination control, so any explicit
  UCAL termination request returns `unsupported`.
- It exposes no non-acknowledged operating mode, so `no_ack=true` returns
  `unsupported`.
- A successful write is not proof of physical CAN completion. The endpoint therefore
  omits `capability_tx_completion` and emits no synthetic completion event.
- Public controller status does not provide authoritative TEC/REC values. UCAL marks
  bus state valid and error counters invalid.
- PLIN is a different API and is not folded into this CAN backend.
- A valid precise tuple that cannot be represented by the PCAN controller timing
  limits returns `unsupported`. The backend does not round its sample point or SJW.

## Echo-mode implementation boundary

Set `UCAL_PCAN_ALLOW_ECHO_FRAMES=1` only for a diagnostic run that needs PEAK's
official self-sent echo frames. The backend enables `PCAN_ALLOW_ECHO_FRAMES` after
channel initialization, reads the value back, and fails the open if the runtime cannot
honor it. Echo frames are delivered as normal received events with
`frame_self_received`; they are not promoted to transaction-correlated transmit
completion events. The default remains off so ordinary CANoe and PCAN-View sessions do
not unexpectedly receive every frame they send.

The backend also detects duplicate GUID/controller tuples during one enumeration.
Rather than collapsing two live channels and opening the wrong one, it adds the public
PCAN channel handle to every colliding endpoint identity and exposes that handle as the
fallback physical index. Once a duplicate tuple has been observed, that choice is retained
for the Broker lifetime: removing one member therefore leaves both the surviving endpoint
and the missing endpoint tombstone at their existing UUIDs instead of compacting the
catalog. This is still an explicitly degraded identity path: PCAN documents that handle
assignment can change across host restarts. Production mappings should use
distinct vendor-stable GUID/controller or configured device identifiers whenever the
hardware/runtime supplies them; Switch must keep an old fallback mapping as an offline
tombstone instead of silently rebinding it.

Classic requests containing only a bitrate prefer 75% sampling. The SJA1000
timing solver selects an exact 75% solution when available, then the closest
valid sample point at the requested bitrate. Explicit controller tuples retain
their timing, including the public 500 kbit/s preset at 87.5%.

## Validation state

- Release and Debug builds pass for x64 and Win32.
- The machine-installed official x86/x64 runtimes load and enumerate attached channels.
  The physical data-plane evidence below is separately bounded to both installed
  architectures and two cross-connected PCAN-USB FD channels.
- The deterministic runtime covers classic 500 kbit/s, an explicit 75% sample point,
  ISO CAN FD/BRS at 500 kbit/s / 2 Mbit/s, exact readback, timestamped self-receive,
  and rejection of the former divide-by-two regression tuple in x86 and x64.
- Deterministic duplicate DeviceGUID/controller fixtures verify that the collision-safe
  fallback keeps endpoints distinct, preserves the surviving ID plus offline tombstone
  during one-member removal/restoration, and clearly marks the public channel-handle
  identity as degraded across host restarts.
- An original test-only ABI runtime validates two colliding FD-capable channels, GUID identity,
  exact FD and classic timing, status validity, write/read/timestamp, close, rejected
  unsupported controls, and absence of invented Tx completion.
- Deterministic x86/x64 negative cases make nominal/data readback unavailable or return
  a mismatching value; all are rejected at open before a lease is issued.
- Deterministic x86/x64 negative cases also reject an unavailable bus-off-auto-reset set,
  unavailable readback, or mismatching readback. The test runtime starts every open with
  auto-reset enabled and refuses writes until UCAL explicitly disables it.
- The deterministic runtime records its first caller thread and rejects any later
  PCAN-Basic call from a different thread in x86 and x64. This prevents a return to the
  physical handle-retention pattern exposed by changing Broker worker threads.
- The ordinary x64 and x86 XL frontend passes through the Broker to that backend and
  receives the transmitted test frame without receiving a false XL `TX_OK`.
- An unmodified python-can Vector client also opened the x64 XL frontend with its
  default 70% nominal/data sample points and completed a CAN FD+BRS transmit/receive
  loop through the test backend. This covers a real third-party XL call sequence rather
  than only the repository harness.

The current physical x64 and Win32 matrices each pass 67/67 cases:

- repeated discovery and alternating Classic/FD open/status/close cycles;
- Classic CAN at 125/250/500/1000 kbit/s and ISO CAN FD/BRS with exact public
  nominal/data-rate readback across the tested 250/500/1000 kbit/s nominal and
  1/2/4 Mbit/s data combinations;
- every Classic CAN and CAN FD DLC length, standard/extended data frames, standard and
  extended RTR, standard/extended filter match and reject, and electrical listen-only;
- 5000 frames/direction simultaneous Classic traffic and 5000 64-byte FD/BRS
  frames/direction with zero loss, duplication, reordering, or synthetic completion;
- same-process correlated request/response latency after the runtime-dispatch fix:
  Classic direction p95 3.037/2.971 ms and FD/BRS direction p95 3.074/2.933 ms,
  with every receive timestamped and both profiles below the declared 10 ms gate;
- bounded 20,000-frame/direction Classic and 64-byte FD/BRS pressure at the sustainable
  one-millisecond submission profile, with zero traffic errors and bounded resources;
- a pure-ACK-loss campaign that truthfully reached warning, observed timestamped
  retransmission at a listen-only peer, and returned to normal peer traffic after
  close/reopen. It did not claim passive or bus-off.

The installed runtime is called only from one dedicated backend dispatch thread.
Before that boundary, repeated physical opens from changing Broker workers retained
runtime handles. With dispatch affinity, the 20-cycle physical lifecycle run warmed
from 186 handles to 194 by cycle two and then stayed exactly at 194 through cycle 20;
the process thread count also stayed flat. The formal 64-byte FD/BRS soak ran for
31 minutes 41 seconds, submitted and received 90,000 exact timestamped frames in each
direction, and closed both channels plus the Broker without a forced stop. Five-minute
samples show one-time warm-up followed by a plateau; Broker private-memory trend was
213,188 bytes/minute, private growth stayed below 10 MB, and handle growth was two.

Two independent PCAN-USB Pro FD adapters connected CAN1 to CAN1 also passed
CANoe 11.0.42 x64 through saved Manager applications. Classic 500 kbit/s and
ISO CAN FD+BRS 500 kbit/s / 2 Mbit/s, 64-byte frames passed 16 frames in each
direction, including FD-to-Classic mode switching and normal channel release.
The CANoe 11 explicit-timing run retained 500 kbit/s with changed Classic BTR
timing and FD nominal/data sample points of 80%. The FD native initialization
used an 80 MHz clock with nominal TSEG1/TSEG2 127/32 and data TSEG1/TSEG2 31/8,
preserving the application timing. Native speed readback returned 500 kbit/s
and 2 Mbit/s, followed by successful duplex traffic.

Remaining physical gates are a safe bit-error topology for passive/bus-off, physical USB
power removal/restoration, host-restart stability of degraded handle identities, and
fault containment across two independent PEAK adapters. Mock self-receive and queue
acceptance cannot replace those tests or create a physical Tx-completion claim.

Manager supplies native runtime overrides from its user settings directory,
`data/backend-plugins/<backend-id>.ini`. The optional `[runtime] library` value
selects an installed vendor runtime; an absent/empty value uses normal discovery.
The module manifest and installed adapter directory remain unchanged during use.
