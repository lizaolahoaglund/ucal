# UCAL Manager

Extract this package into a writable folder and run `UcalManager.exe`.

1. Install the licensed software and the original hardware drivers you intend to use.
2. In Manager, scan software installations and enable the required hardware backends.
3. Create an application profile, select its software installation and bind logical
   channels to available hardware endpoints.
4. Check the profile's compatibility status and launch it through Manager.

Supported versions and capabilities vary by integration. CANoe's application-local
compatibility profile needs its prepared local compatibility environment; its
Provider profile remains a separate candidate. PCAN-View support is limited to its
qualified compatibility profiles. The package does not contain vendor applications,
vendor driver runtimes, or SDK material.

Keep the `data` directory when replacing this program folder. It contains your
settings, profiles and generated shortcut icons. An optional native runtime override
belongs in `data/backend-plugins/<backend-id>.ini`, with the installed driver DLL's
absolute path as `[runtime] library=...`. Normal driver discovery needs no such file.
Application channel mappings belong to their profiles, independently of these
per-machine backend settings.

Desktop shortcuts use the installed application's icon with a UCAL badge. Manager
generates the composite icon locally; no separate vendor icon collection is needed.
Moving the program folder may require recreating its shortcuts.

Project permissions are in `LICENSE` and `LICENSE_SCOPE.md`. Dependency notices are
in `licenses/ThirdPartyNotices.txt`. The companion source attachment named in `SOURCE.txt`
contains the exact covered public source for this build. Developer tools are
maintained in the source repository and are not part of this user package.
