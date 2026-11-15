# Frontends

Frontends implement external software's entry contracts and translate them into UCAL. Hardware backends translate in the opposite direction, from UCAL into vendor hardware APIs.

A frontend can provide an API contract alone, or include an installed-software discovery/launch profile:

- [`vxlapi`](vxlapi/doc/README.md): independent XL API DLLs, a public discovery sidecar and reversible deployment. The matching support runtime is supplied as a binary.
- `pcan_basic`: independent PCAN-Basic API DLL, declarations and reversible deployment.
- [`canoe`](canoe/doc/Launch.md): the public Manager plugin for CANoe discovery and launch. Its managed launch copy uses the separately distributed CANoe compatibility binaries.
- `pcanview`: PCAN-View software profile; its separate binary support package covers the documented executable versions.
- Optional API-only groups provide their external C exports without claiming an installed-software profile.

Each group is independently removable and owns its implementation, declarations, metadata, build files and resources. A source descriptor with kind `api` identifies an API-only frontend. Manager discovers software templates from `frontend.json`; an API-only DLL does not create a software installation or application row. API translation evidence is separate from installed-application acceptance. See [layout](../doc/layout.md) and [support boundaries](../doc/support.md).
