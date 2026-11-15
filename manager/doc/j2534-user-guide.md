# Using J2534 channels in UCAL Manager

## Prepare the backend package

1. Install the hardware driver and its J2534 support package.
2. Put the complete UCAL backend package supplied for that driver in its own folder under `backends/`.
3. Open Manager and select **Drivers**. Select **Rescan** if you added the package while Manager was running.
4. Check the driver name and status. For packages that provide a configuration tool, select **Details** and **Open vendor configuration tool**.
5. Use the green **Enabled** and red **Disabled** buttons to change the driver's state. Manager remembers the setting in the portable data directory.

The package declares its interface type, DLL path, discovery module and dependencies. Manager selects the shared J2534 adapter automatically. See [Plugin packages](plugin-packages.md) for the descriptor format.

A J2534 package can expose several physical channels. PEAK and Vector J2534 packages appear as separate drivers from their native PCAN-Basic and Vector XL backends. The available channels depend on the driver, hardware and discovery module. See the [support matrix](../../doc/support.md) for tested combinations.

## Find the hardware

1. Open **Hardware** and select **Rescan** if needed.
2. Find the online channel by driver, device and port.
3. To narrow the list, enter the driver, device, serial number or channel number in the filter. Separate terms with spaces. Clear the field to show all channels.
4. The application count shows which applications reference the channel. To change a mapping, open that application's **Hardware Channels** settings.

When the vendor configuration tool requires an idle device, close applications using its channels before opening the tool. Reconnect and rescan as required by the vendor. On an operation failure, check the reported operation and error code. Drivers may support setting a parameter even when they cannot read it back.

## Create an application

1. Open **Software** and select **Rescan**.
2. If the target installation is missing, select **Add EXE** and choose its executable.
3. Select **Create application** on that installation's card.
4. Name the application and open **Hardware Channels**.

You can create several applications for one software installation, each with its own channel mapping. Start them from **Applications**.

### Map CANoe channels

1. Choose **Selected channels** in **Hardware Channels**.
2. In the **Hardware endpoint** column for CAN 1, select the desired J2534 endpoint.
3. Use **Add channel** to add rows and select the remaining endpoints. Each row maps one logical CAN channel to one physical port.
4. Check the order and online status. For example, logical CAN 1 can map to the device's physical CAN 3.
5. Open **Integration and Launch** and browse for the CANoe `.cfg` you want to open. Enter any additional startup arguments and select **Save application**.

CANoe supports up to 16 mapped channels. **All compatible channels** selects up to the first 16 in stable order. Use **Selected channels** when the physical ports or their order matter. Removing a mapping renumbers subsequent logical rows and leaves the physical port available.

### Map one PCAN-View channel

1. Select **Create application** on the PCAN-View installation card.
2. In **Hardware Channels**, select the J2534 endpoint for CAN 1.
3. Select **Save application**.

**All compatible channels** picks the first compatible endpoint in stable order. Each PCAN-View instance uses one channel. To use multiple ports, create and launch a separate application for each port. To change the port of an existing application, close it before editing its mapping. [PCAN-View operating instructions](https://www.peak-system.com/produktcd/Pdf/English/PCAN-USB_UserMan_eng.pdf)

## Launch and connect

1. In **Applications**, find the saved application and check that its mapped channels are online.
2. Select **Launch**. If another application holds the target port, close it before connecting.
3. In CANoe, select the CAN or CAN FD timing in its channel and controller settings, then start measurement.
4. In PCAN-View, pick the channel shown by UCAL in the Connect dialog. Set CAN FD, the nominal and data bitrates and the sample points, then select **OK**.
5. In the running software, check for incoming frames. Use its transmit controls to send frames.

Configure bitrate, sample point, termination, operating mode and hardware filters in the external software when it provides those controls. Use the driver's documented controls for settings that it owns. A display filter changes the visible frames only.

If a driver supplies fixed CAN FD sample points, use driver settings and a backend package that match the timing required by the bus. An 8 Mbit/s data rate is one example. The usable rate depends on the driver, adapters and physical bus. See the [backend package contract](../../ucal/doc/backend-plugins.md) for fixed timing declarations.

## Common operations

| Task or symptom | Action |
| --- | --- |
| Change a physical port | Close the external software, open the application's settings, edit the mapping, save and relaunch. |
| Add a logical channel | Select **Add channel** in Hardware Channels. The frontend determines the limit. |
| Add a J2534 backend | Put the complete package in another folder under `backends/` and rescan Drivers. |
| Add a software adapter | Put the complete frontend package under `frontends/` and rescan Software. |
| A driver is missing | Check the package directory, descriptor and dependencies. |
| No hardware channels appear | Check the connection, vendor configuration and discovery module. |
| A channel is offline | Check the hardware connection and whether the port is in use. The saved mapping remains available for the original port. |
| A configuration tool fails to launch | Check its executable, dependencies and working directory. |
| Duplicate an application | Open the application's **More** menu and select **Duplicate application**. |
| Create a desktop shortcut | Open the application's **More** menu and select **Create desktop shortcut**. |
| Check startup prerequisites | Open the application's **More** menu and select **Check compatibility**. |

**Launch in native environment** starts the external software with its normal vendor configuration. **Launch** uses the saved UCAL mapping. **Stop** ends that application's running session and preserves its configuration. **Delete application** removes the saved configuration.

## Portable data and driver details

Put each J2534 package in `backends/<package>/`. Its `backend.json` declares type `j2534-0404-gmw17753` and names the concrete driver, such as PEAK J2534. Its descriptor can reference the installed vendor runtime, its dependencies and an optional configuration tool. Manager supplies the shared J2534 adapter. **Drivers → Details** shows the vendor DLL's version and location and any configuration tool declared by the package.

Every channel option starts with the driver name, so the same device can be distinguished when it is available through both a native backend and J2534. Keep the portable `data/` directory when upgrading the program. It holds configuration and diagnostics.

Manager rescans hardware at startup. It shows a missing endpoint in a saved mapping as **Not discovered · Kept binding** and preserves that mapping. A new application can select currently discovered endpoints. The driver descriptor describes the plugin, and the runtime supplies device inventory.

Disabling a driver preserves existing mappings and allows an active connection to close normally. While disabled, its endpoints cannot start a new connection. Re-enabling it enumerates the hardware again.

Project management displays membership for custom projects. The default project always includes all applications.

Multichannel behavior depends on the driver. A driver supporting separate Open calls receives a specific selector in `PassThruOpen` for each port and creates each channel with `PassThruConnect`. Reopening a driver's default route continues to select its default port. See [J2534 configuration tools and channel discovery](plugin-packages.md#j2534-configuration-tools-and-channel-discovery).
