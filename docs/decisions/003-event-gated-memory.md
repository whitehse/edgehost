# ADR-003: Event-Gated Memory (scoped X1)

## Status

Accepted

## Date

2026-07-18

## Context

Program Decision X1 requires that **new** edgecore / agent / mobile_core
machines never silently `malloc` after create. Growth must surface as pull
events so the host remains the sole process allocator for core-owned data.

Sibling libraries (shaggy, libwebmap, libharness, pique, …) keep their
documented create-time alloc policies; edgehost must not pretend
`host_alloc` wraps their internals.

## Decision

### Scope

| Component | Rule |
|-----------|------|
| **edgecore data buffers** | After create: **no** `malloc`/`realloc`. Emit `EDGE_EVENT_NEED_ALLOC` / `NEED_REALLOC`; host allocates; `edgecore_provide_buffer`. |
| **edgecore create** | May `calloc` the opaque core object and fixed event ring only. |
| **Host** | All process heap for edgecore data goes through `host_alloc` / `host_realloc` / `host_free` (`src/host/host_alloc.c`). |
| **Siblings** | Unchanged. Pre-size via YAML/config caps (e.g. `http.max_body_bytes`). |

### Protocol

```
edgecore_request_alloc / (internal need)
  → next_event: NEED_ALLOC { alloc_id, buf_kind, size }
host_alloc(size)
  → edgecore_provide_buffer(alloc_id, ptr, size)   # size >= requested
… work …
edgecore_request_realloc(alloc_id, new_size)
  → next_event: NEED_REALLOC { alloc_id, old_ptr, old_size, size }
host_realloc(old_ptr, size)
  → edgecore_provide_buffer(…)
edgecore_detach_buffer → host_free
```

### Ownership

- Core stores pointers only; **host frees**.
- `edgecore_destroy` does **not** free host buffers.
- `alloc_id` is opaque; 0 is invalid.

### Wording (X1)

> edgecore, plugins’ edgehost-owned glue, and new app hosts never silently
> malloc after create; they emit memory events. Sibling libraries retain
> their documented create-time alloc policies. Future optional ADRs may
> migrate siblings.

## Consequences

- Tests can drive alloc without a listen socket (P1.2 smoke).
- Host loop (later) must drain NEED_* before other work that depends on the buffer.
- Fixed max buffer slots (`EDGECORE_MAX_BUFS`); exceeding returns failure (no silent grow of the slot table).

## Alternatives considered

| Option | Why not |
|--------|---------|
| Core calls malloc on grow | Violates X1; hides memory pressure from the host |
| host_alloc wraps shaggy/libwebmap | Dishonest; siblings allocate internally today |
| Callback from core to host allocator | Rejected; pull-event lineage (ADR-002) |
