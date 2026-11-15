# Launch CANoe with UCAL Manager

## Prepare and map an application

Install CANoe and the native Vector runtime required by that installation.
Install the native driver for the selected hardware backend and its UCAL package.
Use CANoe 11 or 12 x64 with the matching UCAL CANoe support binaries.
The [support matrix](../../../doc/support.md) lists the verified versions,
hardware combinations and rates.

1. Open `UcalManager.exe` and select the installed CANoe on the Software page.
2. Create an application and assign each logical CAN channel to the intended
   physical backend endpoint on Hardware Channels.
3. Choose your CANoe configuration (`.cfg`). Manager selects the integration
   automatically and shows it in the Integration field. Check that it shows
   **CANoe / Vector XL** for the application-local RuntimeKernel/XL route.
4. Save the application. Check its channel assignments on the Applications page.
5. Launch that saved application. Manager prepares a local launch copy from the
   installed CANoe and synchronizes its UCAL compatibility modules.
6. In CANoe, verify Channel Mapping and Network Hardware before starting measurement.
   Expand CAN 1 and select Setup to configure timing. Disable unused networks in
   the CANoe configuration when they have no assigned hardware.

The [Manager guide](../../../manager/doc/j2534-user-guide.md) describes
application and channel-mapping controls.

## Configure timing

CANoe Setup owns bitrate, sample-point and controller-timing inputs. CANoe 11
Classic mode offers BTR0/BTR1. CAN FD mode offers nominal/data rates, sampling
and advanced timing. Keep both controllers on the wired bus at compatible rates.

UCAL preserves explicit timing requests. Advanced-capable backends translate
controller tuples into their native format. Other backends receive bitrate
and whole-percent sample-point targets rounded from the requested sampling
position, then select their controller timing. For example, a saved Classic
13/2 tuple requests 87.5% and becomes an 88% target on those backends.
Unsupported sampling or timing returns an error. A request containing only
bitrate prefers a 75% sample point. The PCAN-Basic Classic backend computes a
75% configuration when available and otherwise uses its closest valid timing.

## Update or rebuild the launch copy

The compatibility check reads prerequisites without changing the launch copy.
After updating the UCAL package, close every CANoe instance using that copy and
launch the original saved application again. Manager synchronizes the current
packaged DLLs before starting CANoe and retains replaced DLLs for recovery.

If the launch copy is damaged, close its CANoe and RuntimeKernel processes.
Locate the installation-specific directory of the launched CANoe.
Manager uses `%LOCALAPPDATA%\UCAL\canoe-shadow`. CANoe 11 and 12 can also use
`%LOCALAPPDATA%\UCAL\c` when the local data path is deep. When creating the
shorter copy, Manager carries over `vcustom.ini` from the existing copy for
the same installation. To rebuild a damaged copy, rename its directory to
preserve it.
Launch the same saved Manager application to rebuild the copy from the installed
software. Its saved hardware mappings remain in Manager. Reapply any CANoe
settings needed from the retained copy. Deleting and recreating a Manager
application can select the same installation-specific launch copy.

## Native driver registration

The application-local profile checks the 64-bit registry view of
`HKEY_LOCAL_MACHINE\SOFTWARE\VECTOR\CANDriver 1.0\Drivers` before compatibility
acceptance and launch. Missing or empty registration produces a repair hint.
Access failures are reported separately. When a VN1610 is connected, inspect
its `Drivers\VN1610` registration as indicated by the message.

Use the official Vector Driver Setup to repair an incomplete native installation,
then repeat the compatibility check. Virtual CAN and other registered devices
are valid candidates. Channel mapping and a successful measurement determine
whether the selected hardware is usable.

## Scope and cleanup

Use the application-local RuntimeKernel/XL profile for CANoe 11 and 12.
The separately named Vector Provider remains a candidate. See the
[Vector frontend profile](../../vxlapi/doc/canoe/README.md).

Stop measurement before closing CANoe. PCAN-Basic reports queued writes, while
actual delivery is checked at the receiving controller. Normal application exit
must release its mapped channels for the next application.
