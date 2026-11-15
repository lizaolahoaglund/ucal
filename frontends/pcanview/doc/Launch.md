# PCAN-View application launch

Manager discovers PCAN-View installations and prepares one selected UCAL channel.
The software profile uses a separately licensed binary support package for the
qualified x64 5.0.2.838 and 6.0.0.1040 engineering comparators. Other versions need
their own qualification.

Install the matching support launcher, support DLL and injected adapter together
under the package's `frontends/pcanview/x64` directory. Manager checks the required
assets before launch. The support module validates the selected image, signature,
architecture and compiled profile; unsupported or changed images fail before
normal application execution. The profile data is part of the support binaries.

The temporary PEAK net is a local carrier; communication and the selected timing
belong to the UCAL hardware endpoint. In PCAN-View, the `@Virtual` connection suffix
identifies this PEAK carrier, including when Manager maps the channel to a physical
Vector port. Manager's mapping selects the actual backend endpoint.
The original PEAK runtime remains a separately
installed dependency. No PEAK software binary is included in UCAL source.

See the [support interface](Support.md), [support scope](../../../doc/support.md)
and [regression entry points](../../../test/Running.md). Installed-product and
physical acceptance are separate from the hardware-free public-core tests.
