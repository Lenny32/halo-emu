# 0001 — Workspace bring-up and native_sim health gate (M0)

**Phase:** 1 — core emulator
**Depends on:** —
**Effort:** S (half day, mostly download time)

## Story

No West workspace exists on the dev machine — this repo is a bare clone of the
manifest repo. Before any emulator work, assemble the workspace and prove that the
pinned Zephyr fork's POSIX architecture (`native_sim` board) actually builds and runs.
This gates the entire emulator effort: if the fork's posix arch is broken, fixing it
forward in the zephyr fork becomes a prerequisite ticket.

## Tasks

1. **ASK THE USER for the workspace location first.** Hard rules for this ticket:
   - do NOT move or modify the existing repo checkout
   - do NOT create any folder outside the user-confirmed workspace path
   - do NOT run `west init -l .` inside the current checkout — with the repo at
     `~/Projects/halo-firmware`, west would make `~/Projects` the workspace topdir and
     `west update` would clone `zephyr/`, `modules/`, `bootloader/` next to the user's
     unrelated projects
2. Create the workspace per `applications/halo/SETUP.md` at the confirmed path only:
   - `mkdir <workspace>`; materialize this repo as `<workspace>/alif` **without moving
     the original** — prefer `git worktree add <workspace>/alif <branch>` (shares the
     object store, branches stay in sync with the main checkout); plain fresh clone is
     the fallback
   - venv at workspace root: `pip install west pyelftools intelhex cryptography click cbor2 'cmake<4.4' ninja`
   - `west init -l alif && west update` (pulls `brilliantlabsAR/halo-zephyr-alif` @ `190f4cb8`, liblc3, littlefs, mbedtls, libmpix, … — multi-GB, all confined to `<workspace>`)
3. Install host toolchain for native_sim (host gcc, NOT the Zephyr SDK) — **system
   package installs: list them and get user approval before running**:
   - `gcc g++ gcc-multilib g++-multilib libsdl2-dev`
   - 32-bit default build needs `libsdl2-dev:i386` (`dpkg --add-architecture i386`)
4. Health gate — build and run three fork samples on `native_sim`:
   - `west build -b native_sim zephyr/samples/hello_world` → runs, prints
   - `west build -b native_sim zephyr/samples/subsys/fs/littlefs` → flash_sim + littlefs mount OK
   - `west build -b native_sim zephyr/samples/drivers/display` → SDL window appears
5. Record findings (fork board path, 32 vs 64-bit viability, chosen workspace path) in
   `emulator/README.md`.
6. Capture steps 2-4 in **`emulator/init.sh`** so the workspace is reproducible from a
   clean machine in one command (idempotent; `--check` re-runs the health gate,
   `--no-apt` / `--skip-sdk` / `--32bit` switches; apt is the only step that touches
   the system and it lists the packages for approval first).

## Key points in code

- `west.yml` — zephyr pin `190f4cb8638f28ba1fe9dfe53b9f9cec54ee02fb` (Zephyr 3.6-era,
  hwmv1 layout → native_sim lives at `zephyr/boards/posix/native_sim/`, NOT
  `boards/native/native_sim`)
- `applications/halo/SETUP.md` — canonical workspace layout and pip pins (CMake < 4.4)
- Target choice: **32-bit `native_sim` primary** (matches Cortex-M55 pointer/long model —
  safer for Lua lightuserdata and `modules/halo/src/mem_manager.c` address arithmetic);
  `native_sim_64` is the fallback if i386 SDL packages are a fight.

## Acceptance criteria

- [ ] `west update` completes; `zephyr/boards/posix/native_sim/` exists in the fork
- [ ] All three samples build and run on the host
- [ ] Decision recorded: 32-bit or 64-bit as primary target
- [ ] `emulator/init.sh` rebuilds the workspace from scratch on a clean machine
