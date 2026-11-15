# Generic CAN hardware acceptance

These opt-in tests exercise the public UCAL device contract. Each hardware module
owns its runtime environment binding and expected evidence policy in
`test/hardware.json`. Vendor-specific bench procedures, native API mocks and
specialized physical scenarios remain in that module's `test` directory.

`Invoke-UcalCanHardwareSmoke.ps1` supports `Plan`, `Discover` and `Execute`.
Plan validates inputs without opening devices. Discover starts an owned Broker
and lists current endpoints without sending CAN traffic. Execute requires an exact
endpoint UUID and sends a bounded sequence. Optional peer UUIDs select an
independent receive or listen-only endpoint. Bitrates, sample points, modes,
termination, filters and evidence requirements are explicit command arguments;
unsupported device operations must fail truthfully.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ucal/test/hardware/Invoke-UcalCanHardwareSmoke.ps1 `
  -Backend '<installed-backend-selector>' -Mode Plan `
  -BrokerPath out/x64/Release/UcalBroker.exe -HarnessPath out/x64/Release/UcalHarness.exe
```

The runner records exact input artifacts, requested timing, endpoint identity,
received data and the selected completion evidence. Queue acceptance is not
physical transmit completion. Driver configuration acceptance is not a timing
readback. The owning hardware module determines which contract its native runtime
can provide; live capability and return-value checks still apply.

`Invoke-UcalCanFullDuplexAcceptance.ps1` requires two distinct exact endpoints and
an explicitly selected backend/runtime. It audits sequence, payload, duplication,
loss, timestamps and supported completion evidence in both directions.

Installed-application tests belong to each frontend's `test` directory. No default
hardware-independent regression selects installed vendor runtimes or physical
devices. Generated captures and station configuration stay under ignored output.
