# ADR-012: Agent-Ready Documentation

## Status

Accepted

## Date

2026-07-18

## Context

Edge Platform work is multi-repo and agent-driven. Sibling libraries use a
progressive-disclosure doc set so humans and agents can orient without loading
full sources. edgehost must match that contract from P1.1 onward.

## Decision

Adopt (and keep current) the following layout:

| Path | Role |
|------|------|
| `AGENTS.md` | Entry: identity, commands, pins, operating rules |
| `ARCHITECTURE.md` | Core/host split, absences, dependency map |
| `TODO.md` | Living Track 1 PR checklist |
| `docs/DOMAIN.md` | Glossary |
| `docs/decisions/` | ADRs written **when real work lands** (not 12 empty stubs) |
| `docs/guides/` | How-tos as they appear |
| `docs/README.md` | Documentation index |
| `README.md` | Human-facing project summary |

ADR policy: **write the decision when the code lands**. P1.1 ships ADRs
001–002 and 011–012 only; 003 (host_alloc), 005 (HUP), 007 (state), 013
(roles), etc. arrive with their PRs.

## Consequences

- Agents start at `AGENTS.md` and only deep-dive ADRs for the active PR.
- Documentation changes review like code.
- Empty “reserved” ADR files are forbidden.

## Alternatives considered

| Option | Why not |
|--------|---------|
| Single large README | Crowds context; rots |
| Pre-create ADR-001…012 stubs | Program design forbids; decisions without code lie |
| Docs only in headers | No navigational structure for multi-module host |
