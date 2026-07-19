# AGENTS.md — edgehost

**Project identity**: Multi-plugin **io_uring** webserver for the Edge Platform.
Syscall-free **`libedgecore`** (buffers + events) plus a Linux host that owns
sockets, files, signals, TLS (**mbedTLS**), and process malloc. Composes sibling
protocol libraries (shaggy, libyaml, librest, …) — does not reimplement them.

**Program track**: Track 1 (`edge-platform-program-design.md`).  
**Current milestone**: **P1.0** — `deps/pins.txt` + CMake `Find*` modules.

## Key commands

```bash
cmake -B build -S . && cmake --build build
ctest --test-dir build --output-on-failure
deps/verify_pins.sh              # local SHAs vs deps/pins.txt
deps/update_pins.sh              # refresh pins from ~/ siblings
```

Optional roots (same pattern as pqproxy):

```bash
cmake -B build -S . \
  -DSIBLING_ROOT=$HOME \
  -DSHAGGY_ROOT=$HOME/shaggy \
  -DLIBYAML_ROOT=$HOME/libyaml \
  -DLIBREST_ROOT=$HOME/librest
```

## Documentation map

| Doc | Role |
|-----|------|
| `AGENTS.md` | This file — start here |
| `ARCHITECTURE.md` | Core vs host split, absences |
| `TODO.md` | PR track checklist |
| `docs/DOMAIN.md` | Glossary |
| `docs/README.md` | Index |
| `deps/pins.txt` | Known-good sibling SHAs |
| Program design | `~/edge-platform-program-design.md` |

## Dependency pins (P1.0 — read this)

| Rule | Detail |
|------|--------|
| **Pin file** | `deps/pins.txt` lists `repo git-sha` for CI-known-good siblings |
| **Layout** | Develop against checkouts under `$HOME` (or `SIBLING_ROOT`) |
| **CMake** | `cmake/Find*.cmake` accept `*_ROOT` (e.g. `SHAGGY_ROOT`) |
| **Update** | `deps/update_pins.sh` after intentional sibling upgrades |
| **Verify** | `deps/verify_pins.sh` / ctest `edgehost_verify_pins` |
| **libsim** | Not required until Track 0 P0.1; `FindLibsim` is soft |

Do **not** vendor sibling sources into this repo. Link against pins or local roots.

## Operating rules

- **edgecore** (when added in P1.1+): no syscalls; no silent `malloc` after create
  (emit `NEED_ALLOC` / host_alloc — ADR-003 / X1).
- **Host** (`src/host/*`): sole place for io_uring, sockets, files, process malloc, signals.
- Compose shaggy/librest/libyaml/pique/libslack/libteams/libharness — do not reimplement wire protocols.
- C11, CMake ≥ 3.20, `-Wall -Wextra -Wpedantic -Werror`.
- Prefer small, reviewable PRs matching the program PR plan (P1.0 → P1.1 → …).

## Definition of done (any ticket)

- Configures and builds under strict warnings.
- `ctest` passes.
- Pins/docs updated if dependency set changes.
- No new syscalls in core paths (when core exists).

## Current status

**P1.0 complete**: pins file, Find modules, deps smoke test, agent docs.  
**Next**: **P1.1** — edgecore skeleton (`create` / `next_event` smoke) + ADRs 001–002, 011–012.
