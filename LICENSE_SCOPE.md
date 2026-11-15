# License scope

Original UCAL core, Manager, public hardware backend, public frontend,
tool, test and documentation code is licensed under **LGPL-3.0-only WITH
Independent-modules-exception**. The LGPL text is in [LICENSE](LICENSE), its
incorporated GPL text is in [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt), and the
additional permission is in
[licenses/Independent-modules-exception.txt](licenses/Independent-modules-exception.txt).
The respective copyright holders retain ownership.

The additional permission allows independent software applications and plugins to
link with UCAL's public components, statically or dynamically, under their own
licenses. It does not remove the obligation to provide corresponding source for
the covered UCAL components and modifications when distributing their binaries.
It does not relicense third-party software.

Source explicitly identified as private in module publication metadata is outside
this grant. Compatibility support binaries have their own distribution terms;
their presence beside UCAL does not place their implementation under this license.
Public API declarations and serialized/plugin interfaces remain available under
the public-component license. See the [architecture](doc/architecture.md).

Vendor applications, drivers, SDKs, runtime libraries, names and trademarks retain
their respective licenses and ownership. This repository grants no permission to
redistribute vendor software or to use a vendor's trademarks as an endorsement.

When distributing binaries, include these license files, applicable dependency
notices, and the corresponding-source delivery information for the exact build.
See [third-party notices](THIRD_PARTY_NOTICES.md).
