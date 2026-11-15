# Hardware backends

| Folder | Role |
| --- | --- |
| `pcan_basic` | Native PEAK PCAN-Basic driver adapter |
| `vxlapi` | Native Vector VXLAPI hardware adapter; distinct from the application-facing frontend with the same component name |
| `peak_j2534` | PEAK J2534 channel-discovery adapter |
| `vector_j2534` | Vector J2534 channel-discovery adapter |

Each source group owns its original declarations, build files and metadata. A native backend has `backend.json` / `backend.ini` at its root. A J2534 discovery group works with the neutral standard transport and a separately supplied vendor runtime. It is independent of the corresponding native backend.

Private source groups can be supplied alongside these public groups. Source-package visibility controls public assembly. Removing any concrete group must leave the core, Manager and other groups buildable. See [layout](../doc/layout.md) and [backend packages](../ucal/doc/backend-plugins.md).
