# Architecture decision records

This folder holds the architecture decision records (ADRs) for NVRHI as used in the ethereal tree
(`donut/nvrhi`, branch `lithereal-dev`).

An ADR records one significant design decision: the problem, the choice, what it costs, and what else
was considered. ADRs are kept even after the decision is replaced, so that a later reader can see why
the code looks the way it does. Implementation plans and their execution logs live in
[`../plans`](../plans/README.md). An ADR states *what* was decided and *why*; a plan states *how* the
work was carried out.

## When to write one

Write an ADR when a change:

- alters a public interface, a base class or the object model;
- changes ownership, lifetime or ABI rules that backend or consumer code must follow;
- picks one of several reasonable designs and the reason is not obvious from the code.

Do not write one for local refactors, bug fixes or formatting.

## Numbering and file names

- Files are named `NNNN-<kebab-case-title>.md`, with a four-digit number that is never reused.
- Take the next free number. Numbers are assigned in order of writing, not of importance.
- Folder and file names follow the repository naming rules: kebab-case, never camelCase.

## Status

Each ADR has exactly one status:

| Status | Meaning |
| --- | --- |
| Proposed | Under discussion. The code may not follow it yet. |
| Accepted | In force. The code follows it. |
| Superseded by NNNN | Replaced by ADR NNNN. The text is kept unchanged except for this line. |

An accepted ADR is not rewritten when the decision changes. Write a new ADR, set the old one to
"Superseded by NNNN", and link back from the new one. Corrections of fact (a wrong path, a wrong hash)
may be made in place.

## Template

The format follows Michael Nygard's ADR format, with explicit sections for alternatives and references.

```markdown
# NNNN. Title in the imperative or as a noun phrase

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context

The forces at play: the problem, constraints, and relevant facts about the code.

## Decision

What was decided, stated so that a reviewer can check code against it.

## Consequences

### Positive
### Negative
### Risks

## Alternatives considered

Each alternative and why it was rejected.

## References

Commits (with repository), files, related ADRs and plans.
```

## Index

| ADR | Title | Status | Date |
| --- | --- | --- | --- |
| [0001](0001-com-object-model-for-nvrhi.md) | Adopt a COM-like object model in NVRHI (`nvrhi::core`) | Accepted | 2026-10-02 |
| [0002](0002-irhiobject-root-interface-and-iids.md) | `IRHIObject : IObject` replaces `IResource`; every public interface has an IID | Accepted | 2026-10-02 |
| [0003](0003-queryinterface-interface-chains.md) | QueryInterface across interface chains | Superseded by [0007](0007-explicit-queryinterface-tables.md) | 2026-10-02 |
| [0004](0004-autoptr-replaces-refcountptr.md) | `AutoPtr<T>` replaces `RefCountPtr<T>`, including for native COM objects | Accepted | 2026-10-02 |
| [0005](0005-backends-use-monoptr-and-autoptr.md) | Backends use `MonoPtr` / `AutoPtr` instead of std smart pointers | Accepted | 2026-10-02 |
| [0006](0006-no-rtti-queryinterface-casts.md) | NVRHI is built without RTTI; QueryInterface replaces `dynamic_cast` | Accepted | 2026-10-02 |
| [0007](0007-explicit-queryinterface-tables.md) | Explicit QueryInterface tables only | Accepted | 2026-10-02 |

The implementation record for ADRs 0001-0005 is
[`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md).
ADRs 0006 and 0007 are follow-ups to that refactor and record their own implementation and verification.
ADR 0007 has an appendix, [`0007-explicit-queryinterface-tables-audit.md`](0007-explicit-queryinterface-tables-audit.md)
(the per-class QueryInterface audit); it is part of ADR 0007, not a separate record.
