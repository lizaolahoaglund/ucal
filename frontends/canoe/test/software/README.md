# Installed CANoe loading regression

Run the shared Python frame-verdict regression from a source checkout with only
Python's standard library:

```powershell
python -B frontends/canoe/test/software/test_physical_traffic.py -v
```

It calls the installed runner's `verify_traffic` with simulated frames and isolated
hardware, UI and process dependencies. Five tests cover 36 Vector/PCAN and
Classic/FD scenarios, including valid bidirectional sequences and rejection of
wrong payloads, out-of-order sequences, incorrect FDF/BRS flags and zero, negative,
NaN or infinite timestamps. It requires no vendor installation or native runtime.

The hardware-free traffic verdict regression uses the original virtual backend
fixture and the same machine assertions as installed traffic acceptance.

```powershell
./frontends/canoe/test/Test-UcalCanoeTrafficAssertions.ps1 `
  -RuntimeDirectory out/x64/Release -EvidenceDirectory out/canoe-traffic-assertions
```

It checks a valid finite sequence and rejects count, order, content, duplicate,
missing-field, measurement-state and process failures. Screenshot capture errors
remain diagnostic and preserve the functional verdict. Installed traffic tests
use owned measurement state and receiver counters automatically. The installed
measurement control requires Python with pywin32, psutil and pywinauto. Their screenshots
support diagnosis and require no image-based decision.

This opt-in Windows test starts an installed CANoe through a built UCAL Manager package with two logical channels backed by the original virtual backend fixture. It accepts the fixed startup disclaimer in the owned process, checks the main window and UCAL compatibility-module loading, then requires a normal exit from its own CANoe and runtime, then stops its private Broker. It does not start a measurement or qualify physical CAN traffic, channel activation or a new CANoe Provider contract.

Prerequisites: the documented CANoe installation and matching installed Vector runtime, a built portable package, matching x64 deterministic native fixtures, and Python with `psutil`, `pywin32` and `pywinauto`. The runtime directory must contain `UcalBroker.exe`, `UcalHarness.exe` and `UcalBackendFixture.dll`.

```powershell
python frontends/canoe/test/software/installed_launch.py `
  --package-root dist `
  --runtime-root out/x64/Release `
  --canoe-exe '<installed-CANoe64.exe>' `
  --integration-adapter vxl-process-route `
  --fresh-launch-copy `
  --evidence-directory out/canoe-installed-regression
```

Use a new evidence directory. The test copies the package and creates an isolated application workspace and backend catalog. `result.json` records the installed version, configured channel count, disclaimer acceptance and loaded UCAL module names. Missing prerequisites or failed checks return a nonzero exit status. Generated evidence remains local.

`--fresh-launch-copy` also isolates local application data and verifies that first
launch prepares a complete copy and loads the package's modules from that copy.
Without it, the installed user's prepared copy is reused. The default is the
application-local RuntimeKernel/XL route. Use `--integration-adapter canoe-provider`
only when testing the separate CANoe 12 Provider registration route. This loading test does not qualify its official registration contract.

## CANoe 11 and 12 with a physical PCAN backend

Use an isolated bus, the installed PEAK driver, an independent Vector or PCAN CAN
port and Python with `python-can`. Prepare a station-local project for each mode
and installed CANoe version with one active
Real Bus channel. Add the original
[Classic CAN CAPL node](../res/UcalLifecycleClassic.can) or
[CAN FD CAPL node](../res/UcalLifecycleFd.can) to the simulation setup for CAN 1.
Use 500 kbit/s nominal for both cases. Use 2 Mbit/s data, BRS, and 80% nominal/data
sample points for FD. Save the configuration in CANoe with the relative CAPL source
in the same fixture directory. Store the vendor project in a local directory.
Start with no `UcalLifecycle.result` in that directory.

Run the versioned regression from the repository root. Supply the projects for
the versions installed on the station. Use a new, short evidence directory.

```powershell
python -B frontends/canoe/test/software/installed_matrix.py `
  --package-root dist --runtime-root out/x64/Release --evidence-directory out/canoe-matrix `
  --pcan-endpoint-id '<endpoint UUID>' --endpoint-label '<Manager channel label>' `
  --vector-peer-channel 0 `
  --canoe12-classic-config '<CANoe 12 Classic project.cfg>' `
  --canoe12-fd-config '<CANoe 12 FD project.cfg>'
```

Use `--canoe11-classic-config` and `--canoe11-fd-config` for CANoe 11. The runner
checks Windows installation records and the standard CANoe directories. Use
`--canoe11-exe` or `--canoe12-exe` to select a specific executable. Each installed
version runs Classic CAN and CAN FD with standard and deep local data paths.
The top-level `result.json` lists both versions and the outcome of each case.
An absent version is `skipped`. If both are absent, the overall result is
`skipped`. A bad explicit path, an incomplete installation or any failed test
reports `failed` and returns a nonzero exit code.

To check the version selection and report without installed applications, run
`python -B frontends/canoe/test/software/test_installed_matrix.py -v`.

To run one case directly, use the single-version entry point:

```powershell
python frontends/canoe/test/software/installed_launch.py `
  --package-root dist --runtime-root out/x64/Release `
  --canoe-exe '<installed CANoe64.exe>' --integration-adapter vxl-process-route `
  --fresh-launch-copy --create-application --configuration '<station .cfg>' `
  --pcan-endpoint-id '<endpoint UUID>' --endpoint-label '<Manager channel label>' `
  --vector-peer-channel 0 --mode fd --evidence-directory out/canoe12-fd
```

Replace channel index 0 with the index of the independent Vector port.
The same cases accept a second independent PCAN adapter with
`--pcan-peer-channel <PCAN_USBBUSn>` as the peer selector.
For Classic CAN use its configuration and `--mode classic`. Keep the two
evidence directories separate. For each mode run once with a short evidence
path and once with `--local-app-data-min-length 111` and a new evidence directory.
This checks startup when the application's local data path is already deep.
The runner copies the project, checks the saved
endpoint, starts and stops the owned measurement, and checks 16 ordered frames
and their data in each direction. The lifecycle log records the fixed startup
disclaimer and flags unexpected dialogs through normal exit.
