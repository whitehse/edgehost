# AGENTS.md — edgehost

**Project identity**: Multi-plugin **io_uring** webserver for the Edge Platform.
Syscall-free **`libedgecore`** (buffers + events) plus a Linux host that owns
sockets, files, signals, TLS (**mbedTLS**), and process malloc. Composes sibling
protocol libraries (shaggy, libyaml, librest, …) — does not reimplement them.

**Program track**: Track 1 (`edge-platform-program-design.md`).  
**Current milestone**: **P1.2** — host_alloc + NEED_ALLOC path + ADR-003.

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
| `docs/decisions/` | ADRs (write when work lands) |
| `deps/pins.txt` | Known-good sibling SHAs |
| Program design | `~/edge-platform-program-design.md` |

## ADRs

| ADR | Title |
|-----|-------|
| [001](docs/decisions/001-pure-c-choice.md) | Pure C11 |
| [002](docs/decisions/002-core-host-split.md) | libedgecore vs host |
| [003](docs/decisions/003-event-gated-memory.md) | Event-gated memory (scoped X1) |
| [011](docs/decisions/011-fuzz-and-sim-class-a.md) | Class A fuzz/sim policy |
| [012](docs/decisions/012-agent-ready-documentation.md) | Doc layout + no stub ADRs |

## Dependency pins

| Rule | Detail |
|------|--------|
| **Pin file** | `deps/pins.txt` lists `repo git-sha` for CI-known-good siblings |
| **Layout** | Develop against checkouts under `$HOME` (or `SIBLING_ROOT`) |
| **CMake** | `cmake/Find*.cmake` accept `*_ROOT` (e.g. `SHAGGY_ROOT`) |
| **Update** | `deps/update_pins.sh` after intentional sibling upgrades |
| **Verify** | `deps/verify_pins.sh` / ctest `edgehost_verify_pins` |
| **libsim** | Soft until class-A fuzz (P1.5); `FindLibsim` available |

Do **not** vendor sibling sources into this repo. Link against pins or local roots.

## Operating rules

- **edgecore** (`src/core/*`): no syscalls; no silent `malloc` after create for
  data buffers — emit `NEED_ALLOC` / `NEED_REALLOC`; host uses `host_alloc`
  then `edgecore_provide_buffer` (ADR-003).
- **Host** (`src/host/*`): sole place for io_uring, sockets, files, process malloc, signals.
- Compose shaggy/librest/libyaml/pique/libslack/libteams/libharness — do not reimplement wire protocols.
- C11, CMake ≥ 3.20, `-Wall -Wextra -Wpedantic -Werror`.
- Prefer small, reviewable PRs matching the program PR plan (P1.0 → P1.1 → …).

## Definition of done (any ticket)

- Configures and builds under strict warnings.
- `ctest` passes.
- Pins/docs updated if dependency set changes.
- No new syscalls in core paths.

## Current status

**P1.2 complete**: `host_alloc` + NEED_ALLOC/NEED_REALLOC provide path + ADR-003.  
**Next**: **P1.3** — YAML load + SIGHUP apply + ADR-005.
