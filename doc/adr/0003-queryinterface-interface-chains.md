# 0003. QueryInterface across interface chains

- Status: Superseded by [0007](0007-explicit-queryinterface-tables.md)
- Date: 2026-10-02

## Context

`ObjectImpl<Bases...>` (`include/nvrhi/core/foundation.h`) generates `QueryInterface` from its base list.
Its root layer, `QIQueryRoot`, builds one static table with an `IObject` identity entry followed by one
entry per base:

- an interface base got an *offset entry* keyed on that interface's IID;
- any other base (a class implementing `QueryInterface`, a mixin) got an entry keyed on "any IID" that
  calls that base's `QueryInterface` directly.

An interface base answered only its own IID. That is enough for `ObjectImpl<IA, IB>`, but NVRHI has
interface chains:

- `nvrhi::d3d12::IDevice : nvrhi::IDevice : IRHIObject : IObject`
- `nvrhi::vulkan::IDevice : nvrhi::IDevice : ...`
- `nvrhi::d3d12::ICommandList : nvrhi::ICommandList : ...`
- `IDescriptorTable : IBindingSet : IRHIObject : ...`
- every interface `: IRHIObject : IObject`

`d3d12::Device : ObjectImpl<d3d12::IDevice>` must answer `d3d12::IID_IDevice`, `nvrhi::IID_IDevice`,
`IID_IRHIObject` and `IID_IObject` with the same object. Without help it answered only the first and
the last.

The tables must stay constant data: a function-local static array whose initializer MSVC cannot fold
gets a thread-safe initialization guard on every call. The existing code already picked table entries
by type for this reason (see the comment above `QIRootEntryFinderOf`).

The approved plan expected explicit `NVRHI_BEGIN_INTERFACE_TABLE` blocks listing every IID of the chain
in each affected class, and in the classes with more than one interface.

## Decision

Let an interface declare its parent, and let the generated table answer the whole declared chain
(`ce4a071`).

**Declaring the parent.** `NVRHI_DECLARE_UUID_TRAITS_DERIVED(Interface, Parent)` (`types.h`) expands
to `NVRHI_DECLARE_UUID_TRAITS(Interface)` plus

```cpp
using NvrhiQIInterfaceLink = ::nvrhi::details::QIInterfaceLink<Interface, Parent>;
```

`QIParentOf<Itf>` (`foundation.h`) reads the link and checks that its `SelfType` is `Itf`. A link
inherited from a parent is ignored, so an interface that uses plain `NVRHI_DECLARE_UUID_TRAITS` keeps
the old behavior: it answers only its own IID (and `IObject`), whatever its parent declares.

**Walking the chain.** `QIChainCast<Itf>(p, riid)` returns `p` if `riid` is `Itf`'s IID; otherwise it
`static_cast`s `p` to the declared parent and recurses, until a type without a declared parent. Each
step is a `static_cast`, so a parent does not have to be the first base of its child. A
`static_assert` requires `Parent` to be a base of `Interface`. `QIChainEntry` wraps it: on a match it
stores the adjusted pointer and calls `AddRef`.

**Root-layer entry.** In `QIQueryRoot`, each base `B` now maps to one of three entries, chosen by
type:

| Base kind | Key (`pIID`) | Finder |
| --- | --- | --- |
| Interface without a declared parent | `&QIIIDOf<B>` | offset entry (`QIEntryFinder<void>`) |
| Interface with a declared parent | `&QIIIDOf<void>` (any IID) | `QIEntryFinder<QIChain<B>>` |
| Other class (implements `QueryInterface`) | `&QIIIDOf<void>` | `QIEntryFinder<B>` |

`QIChain<B>` is an empty tag type. `QIEntryFinder` gets one more `if constexpr` branch
(`QIIsChain<QIB>`) that calls `QIChainEntry`. The selection uses `QIRootEntryFinderOf<B>` (an alias
over `std::conditional_t`), not `?:` or `constexpr` pointer variables. Every entry is still a pair of
address constants plus an offset, so the table is still constant data with no initialization guard.

**Explicit tables.** A hand-written table can list a chain with
`NVRHI_IMPLEMENTS_INTERFACE_CHAIN(Itf)`, which emits the same "any IID" entry with
`QIEntryFinder<QIChain<Itf>, false>`. It is for mixins and other classes that write their own table.

**Order and identity.** The table is walked in base order, and the first entry that answers wins. If two
chains share an ancestor (`ObjectImpl<IDev, IOther>`, both deriving from `IBase`), the ancestor comes
from the first chain. `IObject` always comes from the first base, so identity does not depend on which
interface the query started from.

**Usage in NVRHI.** All 29 public interfaces and `IRHIObject` use `NVRHI_DECLARE_UUID_TRAITS_DERIVED`
([ADR 0002](0002-irhiobject-root-interface-and-iids.md)). Backend classes list only their most derived
interface, for example `class Device final : public ObjectImpl<IDevice>` in `src/d3d12/d3d12-backend.h`
and `class DescriptorTable : public ObjectImpl<IDescriptorTable>`. Non-`IObject` helpers
(`TextureStateExtension`, `BufferStateExtension`, `vulkan::MemoryResource`) are inherited directly, not
listed in `ObjectImpl` (a `static_assert` in `QICheckRootBases` enforces this). No backend class needed
an explicit table for its public interfaces. The classes the plan listed as needing explicit tables
(d3d12 `ShaderLibraryEntry` and `BindlessLayout`, validation `DeviceWrapper` and `CommandListWrapper`)
each implement a single interface and use plain `ObjectImpl<...>`.

**Tests.** `tests/core/test_qi_route.cpp` has 16 GoogleTest cases. Nine predate this change
(`QueryInterfaceRoute.*`: root, pass-through, mixins, weak references, aggregation, name isolation,
legacy tables). Seven were added in `ce4a071` (`QueryInterfaceChain.*`):

| Test | Checks |
| --- | --- |
| `NestedSameNamedInterfaces` | `backend::IDev` and the outer `IDev` get different IIDs (the `NVRHI_IID` forward declaration). |
| `EveryAncestorSameObject` | A three-level chain plus a nested same-named child answers every ancestor with the correctly adjusted pointer, refuses siblings, and balances the reference count. |
| `UndeclaredParentKeepsOldBehavior` | An interface with plain `NVRHI_DECLARE_UUID_TRAITS` does not answer its parent's IID. |
| `TwoChainsFirstWins` | With two chains sharing an ancestor, the first base answers the ancestor and `IObject`. |
| `ParentNotFirstBase` | The chain adjusts the pointer when the parent is not the first base. |
| `ExplicitTableEntry` | `NVRHI_IMPLEMENTS_INTERFACE_CHAIN` in a mixin's own table. |
| `AggregatedChain` | A chain answered through `DelegatingObjectImpl::NonDelegatingQueryInterface`, with the owner's identity. |

`tests/qi-device.cpp` (`54e011f`) repeats the check on real backend objects: device, texture, buffer,
command list and descriptor table, on D3D11, D3D12 and Vulkan, directly and through the validation
layer.

## Consequences

### Positive

- One macro per interface replaces hand-written tables in about 70 backend classes (the count of
  `RefCounter<...>` uses that `a500ab7` replaced with `ObjectImpl<...>`).
- Adding an interface level, or a new backend class, needs no table maintenance. The chain lives with
  the interface declaration, where reviewers look.
- The tables remain constant data, so `QueryInterface` has no initialization guard on the hot path.
- Existing tables and interfaces that do not opt in behave as before; the nine route tests are
  unchanged and pass.

### Negative

- A chained interface costs an indirect call and a chain of IID compares per query, instead of one
  compare. Chains in NVRHI are at most four levels deep.
- The "any IID" key means that, for a chained base, the table cannot reject an unrelated IID by key
  alone; the finder is called.
- More template machinery in `foundation.h` (`QIInterfaceLink`, `QIParentOf`, `QIChain`, `QIChainCast`,
  `QIChainEntry`).

### Risks

- Forgetting `_DERIVED` on a new interface silently drops its ancestors from `QueryInterface`. The
  `UndeclaredParentKeepsOldBehavior` test documents this behavior, but nothing flags it at compile time.
- With two chains sharing an ancestor, the answer for that ancestor depends on base order.

## Alternatives considered

The first alternative is the one in the approved plan. The others are recorded for completeness.

- **Explicit `NVRHI_BEGIN_INTERFACE_TABLE` blocks in every class with a chain** (the plan). Rejected:
  about 70 classes, each repeating its interface's ancestry, which would break silently when an
  interface gains a level.
- **List every ancestor in `ObjectImpl<...>`** (for example `ObjectImpl<d3d12::IDevice, nvrhi::IDevice>`).
  Rejected: the ancestors are already bases through the interface, so they cannot be listed again as
  separate bases without ambiguity.
- **Answer unknown IIDs with `dynamic_cast`.** Rejected: it needs RTTI, cannot map an IID to a type
  without a registry, and is slower than a static walk.
- **Build the chain table at run time.** Rejected: it brings back the initialization guard that the
  table design avoids.

## References

- nvrhi `lithereal-dev`: `ce4a071` core: interface chains for QueryInterface;
  `a500ab7` Re-base the NVRHI interfaces on nvrhi::IObject; `54e011f` tests: QueryInterface on real
  backend objects.
- donut `ethereal-dev` (earlier, related): `4542ac0` fix(core): QIEntryFinder called a non-existent
  `QIB_` instead of the base `QIB`.
- Files: `include/nvrhi/core/types.h` (`NVRHI_DECLARE_UUID_TRAITS_DERIVED`, `QIInterfaceLink`),
  `include/nvrhi/core/foundation.h` (`QIParentOf`, `QIChainCast`, `QIEntryFinder`, `QIQueryRoot`,
  `NVRHI_IMPLEMENTS_INTERFACE_CHAIN`), `tests/core/test_qi_route.cpp`, `tests/qi-device.cpp`.
- Plan: [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md),
  step 3c.
