## Hard rule: everything stays inside `emulator/`

Emulator work must NEVER create or modify files outside `emulator/` — not the
firmware workspace (`~/halo-firmware`), not the user's home. The firmware tree
is consulted **read-only as a development reference** (headers, build
artifacts, devicetree); the emulator's only runtime input is the firmware
binary passed to `halo-emu -f`. All emulator code lives here: the QEMU fork
patches (machine `halo`, peripheral models), the synthetic ROM stub, the
launcher and tools. If a ticket's text asks for an edit outside `emulator/`,
the ticket is wrong: rework it (model the hardware, vendor declarations,
fake at the machine level) and update the ticket file.

Architecture (since the QEMU pivot, 2026-08-24): the emulator is a QEMU
machine model of the Alif Balletto B1 executing real firmware binaries.
See `README.md` (decision + roadmap) and `EMULATOR.md` (hardware reference).
The prior native_sim source-level emulator is archived at git tag
`archive/native-sim`.

## Ticket implementation

- Alway make sure that the previous ticket is finished, if some code is changed ask the user what to do and do not proceed with the implementation.
- Ticket are written like in Jira and Confluence and have an incremental ticket number. (0001-feature-name, 0002-feature-name, ...)
- Always move the ticket from todo to done at the end of the work.
- A ticket always have the details of what to do and the key points in code as well. reference of the code, ...