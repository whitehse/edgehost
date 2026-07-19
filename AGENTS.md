# AGENTS.md — edgehost

**Project identity**: Multi-plugin **io_uring** webserver for the Edge Platform.
Syscall-free **`libedgecore`** (buffers + events) plus a Linux host that owns
sockets, files, signals, TLS (**OpenSSL non-blocking**, P1.13+), and process
malloc. Composes sibling protocol libraries (shaggy, libyaml, librest, …) —
does not reimplement them. **CPE agent** uses **mbedTLS** (separate tree).

**Program track**: Track 1 (`edge-platform-program-design.md`).  
**Current milestone**: **P1.7a** — state store + REST GET/PUT/DELETE.

## Key commands

```bash
cmake -B build -S . && cmake --build build
ctest --test-dir build --output-on-failure
# plain static server (P1.4a)
./build/edgehost --host 127.0.0.1 --port 8080
# or with YAML:
./build/edgehost --config config/edgehost.example.yaml
deps/verify_pins.sh
deps/update_pins.sh
```

Requires: **liburing-dev**, sibling **libyaml** (`~/libyaml/build/libyaml.a`),
sibling **shaggy** HTTP/1 (`~/shaggy/build/libhttp1.a`), sibling **libsim**
(`~/libsim/build/libsim.a`).

Optional libFuzzer (class A):

```bash
cmake -B build-fuzz -S . -DBUILD_FUZZ=ON -DCMAKE_C_COMPILER=clang
cmake --build build-fuzz --target fuzz_edgehost_a
./build-fuzz/fuzz_edgehost_a -max_total_time=30
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
| `config/edgehost.example.yaml` | Example YAML |
| `deps/pins.txt` | Known-good sibling SHAs |
| Program design | `~/edge-platform-program-design.md` |

## ADRs

| ADR | Title |
|-----|-------|
| [001](docs/decisions/001-pure-c-choice.md) | Pure C11 |
| [002](docs/decisions/002-core-host-split.md) | libedgecore vs host |
| [003](docs/decisions/003-event-gated-memory.md) | Event-gated memory (scoped X1) |
| [005](docs/decisions/005-yaml-sighup-apply.md) | YAML + SIGHUP shadow apply |
| [011](docs/decisions/011-fuzz-and-sim-class-a.md) | Class A fuzz/sim policy |
| [012](docs/decisions/012-agent-ready-documentation.md) | Doc layout + no stub ADRs |
| [007](docs/decisions/007-state-store.md) | State store net.core / map.dynamic |
| [014](docs/decisions/014-tls-openssl-edgehost.md) | edgehost OpenSSL NB; CPE mbedTLS |

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
- **Config**: load YAML on host; apply only via `edgecore_apply_config` (startup
  and SIGHUP share this path — ADR-005). HUP handler only sets a flag.
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

**P1.7d complete**: lab session + proxy `X-User` HMAC headers (ADR-013).  
**Next**: **P1.8a** — plugin ABI HTTP/SESSION + PENDING.
