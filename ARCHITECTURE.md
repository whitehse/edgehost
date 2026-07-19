# ARCHITECTURE.md — edgehost

## Status

**P1.7a**: state store (`net.core`, `map.dynamic`) + REST GET/PUT/DELETE/list.

## Layers

| Layer | Path |
|-------|------|
| **edgecore** | events, config apply, buffers, **state_store** |
| **HTTP serve** | health, SPA, packages, **`/api/v1/state/…`** |
| **io_uring** | production sockets |
| **sim_main** | class-A fuzz |

## State REST (lab open)

| Method | Path |
|--------|------|
| GET | `/api/v1/state/{ns}/{key}` |
| PUT | `/api/v1/state/{ns}/{key}` JSON body |
| DELETE | `/api/v1/state/{ns}/{key}` |
| GET | `/api/v1/state/{ns}?prefix=` |

Enabled ns: `net.core`, `map.dynamic`. Disabled stubs: `net.pon`, etc.

## Next

P1.7b WebSocket STATE_CHANGED; P1.7c auth.
