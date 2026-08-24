## Hard rule: everything stays inside `emulator/`

Emulator work must NEVER create or modify files outside `emulator/` — not the
firmware tree (`applications/`, `modules/`, `drivers/`, `prj.conf`, ...), not
the repo root, not the user's home. Emulator code and config are injected at
build time instead: out-of-tree Zephyr module (`emulator/module/`, via
`-DZEPHYR_EXTRA_MODULES`), config/overlay fragments (`emulator/boards/`, via
`-DCONF_FILE` / `-DEXTRA_DTC_OVERLAY_FILE`), wrapped by `emulator/build.sh`.
If a ticket's text asks for an edit outside `emulator/`, the ticket is wrong:
rework it to this pattern (re-declare Kconfig symbols in the module, provide
stub headers/implementations there, etc.) and update the ticket file.

## Ticket implementation

- Alway make sure that the previous ticket is finished, if some code is changed ask the user what to do and do not proceed with the implementation.
- Ticket are written like in Jira and Confluence and have an incremental ticket number. (0001-feature-name, 0002-feature-name, ...)
- Always move the ticket from todo to done at the end of the work.
- A ticket always have the details of what to do and the key points in code as well. reference of the code, ...