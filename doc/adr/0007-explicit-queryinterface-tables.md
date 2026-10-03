# 0007. Explicit QueryInterface tables only

- Status: Accepted
- Date: 2026-10-02
- Supersedes: [0003](0003-queryinterface-interface-chains.md)

## Context

[ADR 0003](0003-queryinterface-interface-chains.md) made `QueryInterface` implicit. An interface declared
its parent with `NVRHI_DECLARE_UUID_TRAITS_DERIVED(Interface, Parent)`, the root layer of
`ObjectImpl<Bases...>` built one table from the base list (`QIQueryRoot`) and walked each declared chain,
and classes with a table of their own ended it with `NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()`, which
routed every miss (and `IObject`) to the layer they derive from (`QIRouteToCore` through
`QITraits::Core`). [ADR 0006](0006-no-rtti-queryinterface-casts.md) then added class IDs through
`NVRHI_CLASS_INTERFACE_TABLE`, which also relied on that route.

This "implicitly recursive QI" broke the COM interface design the object model is meant to follow:

- What an object answers was no longer written anywhere. It was the result of the base list, of which
  interfaces had opted into `_DERIVED`, of the declaration order of mixins, and of where routing ended.
  Reviewing a class's identity meant reading `foundation.h`.
- An interface declaration decided, for every class that implements it, which other IIDs the class
  answers. In COM that is the implementing class's decision.
- `IObject` identity came from whichever layer the route ended in, not from the class's table.
- The machinery (`QIParentOf`, `QIChain`, `QIChainCast`, `QIKind`, `QIDeclarer`-based classification,
  `QIRouteToCore`, root-entry selection by type) was large, subtle (MSVC folding, name isolation), and
  made "a class with no table" a valid, silently working class.

The user decided to drop the implicit design completely and go back to explicit tables, keeping only the
explicit routing entries, the refcount ownership layers and the liveness probe.

## Decision

**Explicit tables only.** Every concrete class writes its `QueryInterface` as a table
(`include/nvrhi/core/foundation.h`, "Interface tables"):

```cpp
NVRHI_CLASS_CLSID(Device, "a396b171-cf43-4e48-ad04-a9ad0233de55")
class Device final : public ObjectImpl<IDevice>          // nvrhi::d3d12::IDevice
{
public:
    NVRHI_DECLARE_UUID_TRAITS(Device)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Device)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::d3d12::IDevice)    // first: an offset entry, answers IObject
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IDevice)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(Device)                       // class ID (checked_cast, ADR 0006)
    NVRHI_END_INTERFACE_TABLE()
    ...
};
```

Rules, stated in the macro comments:

- A table lists every interface the class implements, ancestors included, and the class ID. An
  interface answers only the IIDs listed for it. A weak-referenceable class lists `IWeakReferenceSource`.
- `IObject` is not listed: the walker answers it with the first entry. The first entry must be an offset
  entry (`NVRHI_IMPLEMENTS_INTERFACE` / `_AS`) whose pointer is the object's `IObject` identity
  (`NVRHI_VERIFY` in Debug). A derived class starts with the same interface as its parent's table. An
  offset entry adds its reference through that identity, so an interface whose first base is not an
  `IObject` still works.
- An aggregated object (`DelegatingObjectImpl` / `DelegatingWeakReferenceSourceImpl`) writes a
  non-delegating table (`NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE...`). It refuses `IObject`: the
  identity is the owner's.
- `NVRHI_END_INTERFACE_TABLE()` ends both kinds; the begin macros record which (`_ITNonDelegating`).

**No default `QueryInterface`.** `ObjectImpl` and `WeakReferenceSourceImpl` implement none (at first
through their root and pass-through layers; since the flat base classes, below, directly). The delegating
base classes keep forwarding `QueryInterface`, `AddRef` and `Release` to
the owner (aggregation), and `NonDelegatingQueryInterface` is pure. A class that neither writes nor
inherits a table is abstract, so `MAKE_RC_OBJ` does not compile for it: the compiler finds every class
that needs a table. A class derived directly from a class with a table may reuse it (scene graph leaves,
render passes, file systems) when it adds no interface or class ID, and says so with
`NVRHI_INHERIT_INTERFACE_TABLE()` (see the per-class check below).

**Per-class check** (added after the first commit, nvrhi `9990dba`). Abstractness is a per-hierarchy
check: a derived class inherits its base's table, so a class that adds an interface or a class ID and
forgets its own table still compiled and silently answered its base's set. The user decided to make the
check per class:

- Every macro that writes or declares a table inside the class body
  (`NVRHI_BEGIN_INTERFACE_TABLE_INLINE`, `NVRHI_BEGIN_NON_DELEGATING_INTERFACE_TABLE_INLINE`,
  `NVRHI_DECLARE_INTERFACE_TABLE()`, `NVRHI_DECLARE_NON_DELEGATING_INTERFACE_TABLE()`) also declares a
  member that names the class: `auto NvrhiQITableClass() -> decltype(this)` (for a non-delegating table,
  `NvrhiQINonDelegatingTableClass()`). It is a member function, not a `using` alias, because the
  `DECLARE` macros and the opt-out take no class name: `decltype(this)` names the class without spelling
  it, which also covers class templates (the injected-class-name), classes in nested namespaces and
  classes whose first base is a helper. The two names differ because one class can write both kinds of
  table (a delegating object with its own `QueryInterface` table), and a member cannot be declared twice.
  The out-of-line `NVRHI_BEGIN_..._INTERFACE_TABLE(Class)` is written outside the class, so the in-class
  `DECLARE` macro carries the member.
- The macros also declare `friend struct nvrhi::details::QITableAccess;`. The check reads the member
  through that class, so a table in a `private:` or `protected:` section works, and the access of the
  members after the macro does not change (no `public:` is emitted). The object wrappers (weak
  references) call `QueryInterface` through `QITableAccess` too, so such a class can also be created.
- `NVRHI_INHERIT_INTERFACE_TABLE()` is the explicit opt-out: "this class adds no interface and no class
  ID; its base's table is correct for it". It declares `NvrhiQITableClass()` for the class and a member
  function with a `static_assert(details::QIInheritsTable<Class>)`: the class's `QueryInterface` (or,
  aggregated, `NonDelegatingQueryInterface`) is declared by a base that has a table macro. A non-template
  class is checked where it is defined; a class template where `MakeNewRCObj` creates it.
- `details::QIDeclaresOwnTable<T>` is true when `T`'s member names exactly `T`. `MakeNewRCObj::RcNew` and
  `RcNewDelegating` (every `MAKE_RC_OBJ*`, `MAKE_GENERIC_RC_OBJ*` and `MAKE_RC_DELEGATING*` path)
  `static_assert` it: "T declares no interface table: add NVRHI_BEGIN/END_INTERFACE_TABLE (list its
  interfaces and class ID) or NVRHI_INHERIT_INTERFACE_TABLE()". A class that declares `QueryInterface` or
  `NonDelegatingQueryInterface` itself by hand (`details::QIDeclaresQueryInterface<T>`, no table macro)
  also passes, so hand-written implementations and the test stubs keep compiling; their derived classes
  are still checked.
- Objects created without `MakeNewRCObj` are not checked by it. In nvrhi the only one is vulkan
  `Queue::m_LifetimeTracker` (a by-value `CommandListLifetimeTracker`), which gets an explicit
  `static_assert(QIDeclaresOwnTable<...>)` next to the member. In donut and the samples, by-value members
  and stack instances (the samples' `main` render passes, `PlanarView` / `CompositeView` / `CubemapView`
  members, local draw strategies, local `NativeFileSystem` / `ShaderFactory` / `TextureCache` objects,
  `TextureData` locals) are a documented exemption; their classes all declare their tables, which the
  audit checks.

All 44 donut and 33 etherealsamples classes that inherited a table add no interface and no class ID, so
all of them got `NVRHI_INHERIT_INTERFACE_TABLE()`; no class needed a table of its own, and no class
answered wrongly before. Four sample classes had been missed by the first audit (see the appendix).

**Kept, explicit routing.**

- `NVRHI_IMPLEMENTS_ROUTE_PARENT(Base)`: an entry that calls `Base::QueryInterface` directly (in a
  non-delegating table, `Base::NonDelegatingQueryInterface`). It is the old
  `RouteParentQueryInterface<T, TBase>` again, plus the table kind, still variadic for template bases
  with commas. A `static_assert` rejects a base that implements no `QueryInterface` (an interface, or
  `ObjectImpl<...>`), using `QIDeclarer`; the call would not link.
- `NVRHI_IMPLEMENTS_ROUTE_MEMBER(m)`: the aggregated member's non-delegating table.
- The variadic `ObjectImpl<Bases...>` and the pass-through layer (`Bar : ObjectImpl<Foo>` adds no second
  reference count). They are about refcount ownership, not QI. Bar's table lists its new interfaces and
  either `NVRHI_IMPLEMENTS_ROUTE_PARENT(Foo)` or Foo's interfaces again. (The pass-through layer was
  removed afterwards: Bar derives from Foo directly, see "Flat base classes" below.)
- `NVRHI_IMPLEMENTS_CLASS(Class)` now records the offset of `Class` in the table's class, so a derived
  class can also answer its parent implementation's class ID.
- `NVRHI_CLASS_CLSID` (forward declaration and class ID). `NVRHI_CLASS_INTERFACE_TABLE` is removed; a
  class writes `NVRHI_DECLARE_UUID_TRAITS(Class)` and its table.

**Removed.** `NVRHI_DECLARE_UUID_TRAITS_DERIVED`, `QIInterfaceLink`, `QIParentOf`, `QIHasParent`,
`QIChain`, `QIIsChain`, `QIChainCast`, `QIChainEntry`, `NVRHI_IMPLEMENTS_INTERFACE_CHAIN`,
`NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT`, `NVRHI_END_NON_DELEGATING_INTERFACE_TABLE_ROUTE_PARENT`,
`NVRHI_QI_END_ROUTE_TABLE_`, `QIRouteToCore`, `QIRouteTableQueryInterface`, `QITraits::Core`,
`QIQueryRoot`, `QIIsInterface`, `QIIsPlainInterface`, `QIRootEntryFinderOf`, `QIEntryFinder`, `QIKind`,
`QIHasNonDelegating`, `QIIsDelegating`, `QIIIDOf<void>`, `NVRHI_CLASS_INTERFACE_TABLE`, and the parts of
`QICheckRootBases` that classified bases for routing. `QITraits<K>` keeps only the lifetime family; it
marks a base that owns a reference count (`QIOwnsRefCount`, which now accepts only our own `QITraits`).
(Both went with the layers; see "Flat base classes".)

**Flat base classes** (follow-up, nvrhi `e8a7951`, donut `a0b7909`, aggregate root `d0257db`). The user
decided to remove the layer machinery and use flat variadic bases, rather than go back to the old
single-interface `RefCountedObject<BaseItf>`:

- `ObjectImpl<Bases...>`, `WeakReferenceSourceImpl<Bases...>`, `DelegatingObjectImpl<Bases...>` and
  `DelegatingWeakReferenceSourceImpl<Bases...>` derive directly from `Bases...` (and, as before, from
  `UserAllocated`, now through a marker, below). Each owns its reference count, packed control block or
  owner pointer and implements `AddRef` / `Release`; the delegating ones forward `QueryInterface`,
  `AddRef`, `Release` and `GetWeakReference` to the owner and leave `NonDelegatingQueryInterface` pure.
  Each keeps `NvrhiQIAnswerProbe()`, `DestroyObject`, `Attach`, its friends (`MakeNewRCObj`, the object
  wrappers, `WeakReferenceImpl`, `WeakRefTypeTrait`), `WeakRefImplType`, `Release(TPreObjectDestroy&&)`, the
  virtual destructor of the weak class, the packed and non-packed control-block paths
  (`NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT`) and the deleted copy and move operations. The member code is the
  code of the former root layers; behaviour is unchanged.
- Removed: `QIRootLayer`, `QIPassThroughLayer`, `QILayer`, `QITraits`, `QILifetime`, `QIIsTraits`,
  `QIOwnsRefCount` and `QICheckRootBases`.
- The base-list checks are `static_assert`s in each template, through
  `details::CheckObjectImplBases<Weak, Bases...>()`: at least one base and every base derives from
  `IObject`; a weak object lists `IWeakReferenceSource` or an interface derived from it; and, new, no base
  is an implementation class: "list interfaces only; to build on an implementation Foo, derive from Foo
  directly (class Bar : public Foo) and write Bar's table". The four templates derive from one empty
  marker, `details::ObjectImplTag` (which derives from `UserAllocated`, so no subobject is added and the
  layout is the same as before); `details::IsObjectImpl<T>` is `std::is_base_of_v<ObjectImplTag, T>`, which
  ignores access and is not ambiguous. Interfaces and mixins (classes with their own table that own no
  reference count, e.g. `P1`, `XMix` in the tests) are still valid bases.
- A class that builds on an implementation derives from it directly with public inheritance (so
  `NvrhiQIAnswerProbe()` stays reachable from its table end) and writes its own table
  (`NVRHI_IMPLEMENTS_ROUTE_PARENT(Foo)` plus its additions, or Foo's interfaces re-listed) or
  `NVRHI_INHERIT_INTERFACE_TABLE()`; the per-class check enforces that. Constructors are inherited with
  `using Foo::Foo` or written out.
- Pass-through users: none in production. The full aggregate build (also with GVDB and with audio on)
  needed no source change in nvrhi `src`, donut or etherealsamples; every class that built on an
  implementation already derived from it directly. Only the test fixtures used `ObjectImpl<Foo>`: in
  `tests/core/test_qi_route.cpp`, `Bar` and `BarListed` (`: Foo`), `Leaf` (`: Mid`), `WeakBar`
  (`: WeakFoo`), `InnerEx` (`: Inner`, `using Inner::Inner`) and `QIUser::App::XLeaf` (`: XRoot`); their
  tables were already explicit and did not change.


**The liveness probe** (`details::QIStrongRefProbeIID`, ADR 0006) used to be answered by the root layers
after their automatic table refused the IID. The table now lives in the user class, so the end of the
table answers it:

- On a miss in a `QueryInterface` table with `riid == QIStrongRefProbeIID`, `QITableQueryInterface`
  calls the class's non-virtual `NvrhiQIAnswerProbe()`, found through the base that owns the reference
  count: `ObjectImpl` answers strong count > 0, `WeakReferenceSourceImpl` answers `!IsExpired()`, and the
  delegating base classes forward to the owner (for a delegating class that overrides `QueryInterface`
  with a table of its own). A class without the hook (a mixin with its own table) does not answer; its
  host's table end does. A class derived from an implementation (`Bar : public Foo`) reaches Foo's hook
  through public inheritance.
- A delegating object's `QueryInterface` forwards everything, the probe included, to its owner.
- A non-delegating table never answers the probe: the owner's table does, and answering it there would
  ask the owner, whose table may route back through `NVRHI_IMPLEMENTS_ROUTE_MEMBER`.
- The name follows the repository naming rules (they constrain macros and files; methods are PascalCase)
  and carries the `Nvrhi` prefix so that it does not collide with user members.

The tables stay constant data (address constants only, `QIIIDOf<Itf>` for IIDs, functions as finders), so
MSVC emits no thread-safe initialization guard.

**Interfaces** use plain `NVRHI_DECLARE_UUID_TRAITS(X)` after `NVRHI_IID(X, "...")`. The IIDs did not
change.

**Audit.** Since no chain metadata is left, every object-model class was checked by a script against its
inheritance graph: the IIDs its table answers (own table, inherited table, route-parent entries followed,
route-member entries reported as aggregation) must equal `IObject`, every interface in its graph and the
class IDs of the class and its implementation ancestors. Result: 265 classes, all as expected
(appendix: [0007-explicit-queryinterface-tables-audit.md](0007-explicit-queryinterface-tables-audit.md)).
Re-run for the per-class check with two fixes (classes are now told apart per program, so same-named
classes in different samples are no longer merged, and directories named `nvrhi` outside nvrhi are no
longer skipped): 280 classes; every production class is "own" or "inherit (declared)".
Checked by hand against the old rules (all NVRHI interfaces were `_DERIVED`, so the root layers answered
whole chains; the GVDB interfaces were plain, and their tables listed the ancestors themselves), no class
answered fewer or more interfaces before the change either: the sets are the same, and the change is
structural, not behavioural.

| Repository | Tables added or changed |
| --- | --- |
| nvrhi core | 4 (`DataBlobImpl`, `StringDataBlobImpl`, `ProxyDataBlobImpl`, `ProxyRefDataBlobImpl`: class ID entry is now `NVRHI_IMPLEMENTS_CLASS`, interface first) |
| nvrhi backends | 75: d3d11 15, d3d12 31, vulkan 26, validation 3 (all rewritten; 70 from `NVRHI_CLASS_INTERFACE_TABLE`, 5 pooled types from `..._ROUTE_PARENT()`) |
| donut | 47 (44 new `IObject` / `IWeakReferenceSource` tables, `NativeFileSystem`, `GltfImporter` (+ `AssimpSceneImporter` under `#if 0`), `StbImageBlob`); 43 derived classes inherit a table |
| etherealsamples | 12 (7 new `IObject` tables, `DeviceChild<T>`, `cuda::Device`, `Kernel`, `DepthMapPass`, `GPDeviceMessageCallback`; plus 2 redundant `IObject` entries dropped); 29 derived classes inherit a table |

**Tests.**

- `tests/core/test_qi_route.cpp` (17 cases) is rewritten: explicit root tables, class ID with a helper
  base first, pass-through with `ROUTE_PARENT` and with re-listed interfaces, pass-through over a class
  without a table, an inherited table, mixins in order, weak pass-through, aggregation through a
  delegating pass-through (non-delegating `ROUTE_PARENT`), a delegating object with its own
  `QueryInterface` table, an out-of-line table, name isolation, nested same-named interfaces, every listed
  ancestor answered and unlisted ancestors refused, an interface whose first base is not an `IObject`.
  "A missing table is a compile error" is a set of `static_assert(std::is_abstract_v<...>)`. The probe is
  checked through each kind of table. The per-class check adds `static_assert`s on
  `details::QIDeclaresOwnTable` for an own inline table, an out-of-line table with `DECLARE`, a
  non-delegating table, both kinds in one class, the `INHERIT` opt-out (also over a non-delegating
  table), a derived class without anything (false), class templates (table, opt-out, nothing), a table
  and the opt-out in a private section, an out-of-line table declared in a protected section (with the
  member after the macro still inaccessible), and a hand-written `QueryInterface`; plus three runtime
  cases (private sections, templates and opt-out, delegating opt-out): 20 cases.
  With the flat base classes, the pass-through cases became "derived from the implementation directly"
  cases (`DerivedImplementationRouteParent`, `DerivedImplementationRelisted`, `DerivedFromClassWithoutTable`,
  `WeakDerivedImplementation`, `AggregationWithDerivedDelegatingObject`): one reference count (the same
  count through the derived and the parent's bases), the derived table routes to its parent's table, and
  the probe is answered through the derived class (`details::QIAnswerProbe` reaches the parent's hook). New
  `static_assert`s cover the "a base is not an implementation" rule through the exposed trait
  `details::IsObjectImpl` (interfaces, mixins and helpers false; the four base classes, classes derived from
  them and classes derived from those true) and `details::CheckObjectImplBases` for interface and mixin
  lists. The name-isolation fixture also declares `ObjectImplTag`, `UserAllocated`, `WeakReferenceImpl` and
  `ObjectWrapperStorage` as members of a user interface. Still 20 cases.
- `tests/core/test_checked_cast.cpp` keeps all its cases (destructor, pre-destroy and weak-constructor
  safety) with explicit tables. With the flat base classes it adds `DerivedImplementation`: `DerivedFoo :
  public FooImpl` (route parent) casts to itself and to `FooImpl`, has one reference count, and casts
  itself in its destructor without resurrecting itself (probe through the derived table): 9 cases.
  `tests/core/test_auto_ptr.cpp` checks that `WeakPtr<DerivedObject>` (a class derived from a weak
  implementation) keeps `WeakReferenceImpl` as its control block type.
- `tests/qi-device.cpp` creates device, texture, staging texture, buffer, sampler, event and timer query,
  framebuffer, heap, command list, binding layout and set, bindless layout and descriptor table, and
  checks that each answers exactly its interfaces and its class ID and refuses every other public IID and
  class ID (with the reference count balanced), on D3D11, D3D12 and Vulkan, directly and through the
  validation layer. The class IDs are a copy in the test (the backend headers are internal).

## Consequences

### Positive

- Each class states what it answers, in one place, as COM does. Review is local.
- The interface declarations no longer decide anything about implementations.
- `foundation.h` is smaller and simpler; most of the template machinery is gone.
- A class without a table does not compile when instantiated, instead of silently answering a default set.
- Every class created with `MAKE_RC_OBJ` states its table in its own body: a derived class that forgets
  its table no longer compiles, and reusing the base's table is a visible, reviewed decision
  (`NVRHI_INHERIT_INTERFACE_TABLE()`). The check found two sample classes the first audit had missed.
- A query walks a short table with plain IID compares; no chain walks or "any IID" entries for interfaces.

### Negative

- Tables repeat the ancestry of their interfaces (`IRHIObject` in all 75 backend tables). Adding an
  interface level means editing every implementing class.
- Two lines more per class than `NVRHI_CLASS_INTERFACE_TABLE`, and one table per otherwise plain
  `ObjectImpl<IObject>` base class in donut and the samples.
- `NVRHI_END_INTERFACE_TABLE()` depends on the begin macro's local `_ITNonDelegating`; hand-written
  tables must use the begin macros.
- One more line in every derived class that reuses its base's table (77 in donut and the samples), and
  the table macros add a friend declaration and an inline `NvrhiQITableClass()` member to each class
  (never called, no code).

### Risks

- Resolved: a derived class that adds an interface or a class ID and forgets its own table used to
  compile silently with its base's table. The per-class check rejects it now; what remains is a wrong
  `NVRHI_INHERIT_INTERFACE_TABLE()` (a class that does add something), which is a review question, and
  objects not created through `MakeNewRCObj` (by-value members, stack instances), which the check does
  not see.
- A missing ancestor in a table is not a compile error. `tests/qi-device.cpp` (exact sets, every public
  IID) and the audit script catch it for the backends; donut and sample classes rely on review.
- A table that starts with a class ID or route entry asserts only in Debug.
- A class that derives privately from `ObjectImpl` (or from an implementation class) cannot reach
  `NvrhiQIAnswerProbe()` from the table end, so it does not answer the probe; `checked_cast` then takes
  the safe, type-only branch. Derive publicly. donut `BloomPass`, `SkyPass`, `SsaoPass`, `ToneMappingPass`
  and `AudioCache` were private and were made public in donut `ffc6c24`.

## Alternatives considered

- **Keep the implicit chains (ADR 0003).** Rejected by the user: it broke the COM interface design (see
  Context).
- **A default `QueryInterface` in `ObjectImpl` that answers its direct bases** (the state before ADR
  0003). Rejected by the user: a class without a table would still work silently, and the default answer
  would again not be written in the class.
- **Drop the route entries as well** (every class re-lists its parent's interfaces). Rejected by the user:
  `NVRHI_IMPLEMENTS_ROUTE_PARENT` and `NVRHI_IMPLEMENTS_ROUTE_MEMBER` are explicit entries, and
  aggregation needs the member route.
- **Answer the probe in a virtual hook on `IObject`.** Rejected: it changes the `IObject` ABI. The
  non-virtual hook on the base classes is found at compile time from the table's class.
- **Keep the pass-through layer** (`Bar : ObjectImpl<Foo>`, selected by `QITraits` / `QIOwnsRefCount`).
  Rejected by the user in the follow-up: deriving from `Foo` directly says the same with plain C++ and
  needs no layer selection. **Go back to the single-interface `RefCountedObject<BaseItf>`**: also
  rejected; the flat variadic bases keep multi-interface classes and mixins.

## References

- nvrhi `lithereal-dev`: `5922252` Explicit QueryInterface tables only (drop implicit chain/route-parent
  QI), core and backends in one commit.
- donut `ethereal-dev`: `b2f615c` core: explicit QueryInterface tables (bump nvrhi); `ffc6c24` public
  `ObjectImpl` inheritance in `AudioCache` and the render passes.
- etherealsamples `main`: `2d53121` Explicit QueryInterface tables (nvrhi ADR 0007).
- aggregate root `main`: `818eda4` bump donut, etherealsamples (nvrhi: explicit QueryInterface tables);
  `ece3cfa` bump donut.
- Per-class check: nvrhi `9990dba` core: per-class interface table check (NVRHI_INHERIT_INTERFACE_TABLE);
  donut `3a36fb8`; etherealsamples `54a4610`; aggregate root `5396e23`. Verified with the Release
  aggregate build (also with GVDB and with audio on), Debug and Release ctest (unchanged: only
  `memory_queries_d3d12` fails, 0x80070057), donut unit tests (only the known float-formatting
  failures), the generated `Gen-Interface.ps1` code (`cl /W4 /permissive- /GR-`) and Release smoke runs.
- Flat base classes: nvrhi `e8a7951` core: flat variadic ObjectImpl bases (drop root/pass-through
  layers); donut `a0b7909` (nvrhi bump only); etherealsamples unchanged; aggregate root `d0257db`.
  Verified with the Release aggregate build (also with GVDB and with audio on; no source outside nvrhi
  needed a change), Release and Debug ctest (unchanged: only `memory_queries_d3d12` fails, 0x80070057),
  donut unit tests (only `test_string_utils` and `test_console_interpreter` fail, as before), a scratch
  build in non-packed mode (`NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT=0`) and Release smoke runs.
- The explicit-table form goes back to donut `66698ff` (`include/donut/core/object/Foundation.h`,
  `RouteParentQueryInterface`).
- Files: `include/nvrhi/core/foundation.h`, `include/nvrhi/core/types.h`, `include/nvrhi/core/datablob.h`,
  `src/<backend>/<backend>-backend.h`, `tests/core/test_qi_route.cpp`, `tests/core/test_checked_cast.cpp`,
  `tests/qi-device.cpp`; donut `tools/Gen-Interface.ps1` (emits plain traits and an explicit table;
  `-Ancestors` lists the bases' parents).
- Verification: Release aggregate build, GVDB build, Debug and Release ctest (`nvrhi_test_*`,
  `qi_device_*`, `memory_queries_d3d11/vulkan` pass; `memory_queries_d3d12` fails as before with
  0x80070057), donut unit tests (only the known float-formatting failures), Release smoke runs with and
  without the validation layer.
- Supersedes [0003](0003-queryinterface-interface-chains.md). Updates
  [0002](0002-irhiobject-root-interface-and-iids.md), [0005](0005-backends-use-monoptr-and-autoptr.md)
  and [0006](0006-no-rtti-queryinterface-casts.md). Plan follow-up:
  [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md).
