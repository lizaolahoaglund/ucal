# Vector XL hardware backend

Status: implemented; official Virtual CAN and bounded physical VN1610 CAN 1
Classic CAN / ISO CAN FD+BRS acceptance passed on x86/x64 (2026-10-22).
Broader physical qualification remains explicit below.

## Scope

This backend is the layer-3 adapter from the neutral UCAL contract to a locally
authorized Vector XL runtime. It is independent of the layer-1 Vector frontend family.

Its source group is `backends/vxlapi`; the application-facing API is in
`frontends/vxlapi`. The common component name identifies the API, and the
parent directory identifies its direction. Existing `vector-xl` runtime identity
and saved endpoint mappings retain their contract spelling.

```text
CANoe or an ordinary XL client
  -> UCAL Vector frontend profile
  -> UCAL core/Broker
  -> Vector XL backend
  -> locally installed XL runtime
  -> Vector Virtual CAN or physical Vector hardware
```

No pair-specific CANoe-to-Vector bridge exists. The same frontend also targets another hardware backend or
PCAN, and the same backend accepts any conforming UCAL frontend.

## Selection

Use an explicit runtime path when reproducibility matters:

```powershell
$env:UCAL_BACKEND = 'VECTOR'
$env:UCAL_VECTOR_XL_RUNTIME = '<absolute path to the architecture-matching XL runtime>'
.\out\x64\Release\UcalBroker.exe
```

`UCAL_BACKEND=ALL` merges every backend that initializes successfully. A missing
Vector runtime does not suppress an available PCAN catalog. The runtime and
all generated evidence remain local; no Vector DLL or header is distributed here.

In Manager, enable the Vector XL backend on the Drivers page. Use the
[backend runtime settings](../../../manager/doc/PackageLifecycle.md#settings-and-profiles)
to select a specific installed runtime. Frontend electrical options are capability-aware:
setting CAN termination to false does not require a backend with programmable
termination, while setting it to true remains an explicit unsupported request.

Physical channels with a reported serial number use hardware type, serial number and
physical CAN port as their persistent identity. XL hardware/channel indices, access
masks and display names may change without changing that identity. Virtual channels
use the runtime's virtual adapter identity and port, independently of global XL
channel indices and display names. Connecting physical interfaces must not replace
an existing Virtual CAN mapping. Physical devices without a serial retain an
enumeration-based fallback; no persistent hardware identity is invented for them.

If a saved endpoint cannot be resolved, Manager retains it offline until the user
explicitly rebinds it. A different available port is never substituted automatically.

## Capability contract

| Capability | Result |
|---|---|
| Classical CAN; standard/extended data; RTR | Supported |
| ISO CAN FD and BRS through 64 bytes | Supported on channels advertised as CAN FD |
| Precise nominal/data timing | TSEG/SJW preserved through public XL configuration; API/controller limits apply |
| Normal and listen-only output modes | Supported |
| Standard/extended acceptance predicates | Supported; exact predicates are enforced at the UCAL boundary |
| Rx timestamps | Supported from the official XL event timestamp field |
| Tx completion and Tx failure | Supported as separate, transaction-correlated events |
| Warning/passive/bus-off and TEC/REC | Reported only when supplied by an XL chip-state event |
| Programmable termination | Unsupported |
| No-ACK/self-ACK mode | Unsupported |
| LIN | Unsupported in this first backend |
| Bus-load statistics, TX-off, accelerator/object-buffer/error-generator/private operations, cross-adapter clock sync | Unsupported |

Invalid identifiers, DLC/payload mismatches, classic+BRS, ESI submission, and FD RTR are
rejected before the vendor runtime is called.

The qualified runtime returns eight Classic CAN receipt-key bits and twelve CAN FD
receipt-key bits. UCAL keeps the complete caller transaction ID in its pending map
and rejects a colliding in-flight key as busy until the actual receipt releases it.
This fixes the VN1610 Classic completion mismatch that the previous virtual fixture
did not expose. The updated deterministic fixture reproduces that truncation and
checks full-ID restoration, collision rejection and reuse after completion.

These receipt-key widths are observed runtime behavior. The
[official XL manual](https://www.vector.com/dam/jcr%3A37a98de5-a2ca-4333-b6c7-ead8b088c937/XL_Driver_Library_Manual_EN.2026-09-12-16-40-28.pdf)
marks transaction fields for internal use; the bounded correlation evidence is not
a vendor guarantee across other runtimes or interfaces. Repeat the correlation gate
for those combinations.

## Acceptance evidence

The deterministic x86/x64 runtime verifies enumeration, stable physical and virtual
port identity across XL index/name changes, stable adapter identity,
exclusive ownership, classic and FD/BRS frames, RTR payload semantics, filters,
listen-only mode, invalid-frame rejection, nonzero timestamps, correlated Tx success
and failure, chip state, cleanup, and unsupported controls. The complete repository
suite also runs the ordinary XL and Provider-profile frontends against this backend and
checks `ALL` fallback routing.

A dedicated x86/x64 catalog regression changes XL hardware/channel indices, access
masks and channel names, then checks that serial-backed physical identities remain
stable, different serials/ports remain distinct, and virtual/anonymous fallbacks keep
their existing behavior.

The installed official x86 and x64 runtimes each passed:

- endpoint enumeration and CAN FD open/configure;
- 64-byte CAN FD+BRS transmit completion with full UCAL transaction correlation;
- a nonzero vendor timestamp read from the documented event offset;
- ordinary XL-frontend and Provider-profile data paths;
- installed CANoe discovery, two-channel activation, traffic, Stop/Start, and release.

### Physical VN1610 follow-up, 2026-10-22

The original installed-runtime run exposed Virtual CAN only. The physical VN1610 now
has no Windows Code 10 or XL configuration error and exposes two CAN ports. CAN 1
was wired to an independent CAN/CAN FD peer.
The installed official XL 9.7.26.0 runtimes passed the following bounded matrix:

| Gate | Physical result |
|---|---|
| x64 runtime/client and x86 runtime/client | Each direction: 1000 Classic CAN frames at 500 kbit/s and 1000 64-byte FD+BRS frames at 500 kbit/s / 2 Mbit/s |
| x86 client to the default x64 Broker/runtime | Each direction: 100 Classic frames and 100 64-byte FD+BRS frames |
| Additional Classic nominal bitrates | 250 kbit/s and 1 Mbit/s, 100 frames in each direction per rate |
| Extended identifiers | Classic and 64-byte FD+BRS, 100 frames in each direction per format |
| Classic RTR | DLC 8 with zero payload bytes, one frame in each direction |
| Standard/extended filters | Match and rejection; rejection is paired with an actual physical peer transmit completion |
| Lifecycle and ownership | 12 alternating Classic/FD open/status/close cycles, exclusive busy response, independent CAN 2 configuration, terminated-client cleanup and CAN 1 reopen |
| Listen-only | Configuration/status/close passed; electrical ACK-suppression behavior was not tested |
| Unsupported controls | Explicit programmable termination, no-ACK and internal loopback rejected as unsupported |
| Ordinary XL and existing Provider profile | x86/x64 frontend harnesses each produced an exact peer-observed 64-byte FD+BRS frame and matching completion through the physical backend |

The directional matrix totals 4601 frames in each direction, all with exact IDs,
flags, DLC/payload and timestamp evidence; no duplicate, corrupt or reordered frame
was accepted. Every Vector submission produced its own actual correlated completion.
Repeated-traffic runs ended with valid normal controller state and zero TEC/REC on
both ends. Short single-frame runs can finish before the asynchronous chip-state
reply; their unavailable state remains explicitly invalid.
Every owned lease and Broker closed successfully. Raw identifiers, serial numbers,
commands, runtime hashes and logs remain in ignored local evidence.

Successful public XL timing configuration and physical interoperability are proven;
this is not exact applied-timing register readback. The run does not claim physical
hot-plug/power loss, injected warning/passive/bus-off recovery, simultaneous sustained
full duplex, multi-adapter identity or CAN 2 cable traffic. Installed CANoe/PCAN-View
physical application acceptance remains separate from the frontend harness evidence.

The consolidated result matrix is in
[support scope](../../../doc/support.md).

Manager supplies native runtime overrides from its user settings directory,
`data/backend-plugins/<backend-id>.ini`. The optional `[runtime] library` value
selects an installed vendor runtime; an absent/empty value uses normal discovery.
The module manifest and installed adapter directory remain unchanged during use.
