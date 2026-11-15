# Backend packages

Place each supplied backend package in a separate `backends/<id>` directory. UCAL Manager scans it on startup and on **Rescan**. A card represents a concrete backend, such as a native driver or a particular vendor's J2534 package. The neutral J2534 template itself is not a device card.

`backend.json` supplies static name, version, type, library, configuration and optional discovery/configuration-tool information. A native `type: ucal` library implements `UcalBackendGetApi`; `type: j2534-0404-gmw17753` identifies a vendor J2534 runtime hosted by the shared adapter. A driver runtime and any vendor configuration EXE remain separately licensed dependencies. Do not redistribute them in UCAL source.

J2534 04.04 defines no universal port directory. A concrete package's discovery adapter uses the vendor's documented public selection mechanism. It declares separate device opens or a shared DeviceID with separate Connect routes. A NULL/default open is not multiplied into fictitious physical channels. Each admitted CAN endpoint must qualify ISO CAN FD; Classic CAN sessions remain available on qualified devices.

The Drivers page reports the actual library and optional configuration tool, and saves enable/disable preference in user configuration. Optional read/write support remains independent; failed calls report the actual error. Metadata is static. Runtime channel inventory and online state are not saved into backend.json.

See [step-by-step workflow](../../manager/doc/j2534-user-guide.md), [package contract](../../manager/doc/plugin-packages.md), [source layout](../../doc/layout.md) and [support](../../doc/support.md).
