# 0006. NVRHI is built without RTTI; QueryInterface replaces `dynamic_cast`

- Status: Accepted
- Date: 2026-10-02

## Context

After ADRs [0001](0001-com-object-model-for-nvrhi.md)-[0005](0005-backends-use-monoptr-and-autoptr.md)
every NVRHI object is an `nvrhi::IObject`. Every public interface has an IID, `QueryInterface` answers
whole interface chains (at the time through ADR 0003's implicit chains; since
[ADR 0007](0007-explicit-queryinterface-tables.md) through explicit tables), and the backend classes
derive from `ObjectImpl<IX>`. NVRHI still needed RTTI in
11 places:

- `checked_cast<T>(U)` in `include/nvrhi/common/misc.h`. Debug builds used `dynamic_cast` plus an
  assert, Release builds a `static_cast`. It has 411 call sites in `src/`: d3d11 68, d3d12 188,
  vulkan 151, validation 4. Most cast an interface to a backend class (`checked_cast<Texture*>(ITexture*)`).
  A few cast `IRHIObject*` to an interface (`BindingSetItem::resourceHandle`, `unwrapResource`).
- 10 unconditional `dynamic_cast`s in the validation layer. They are runtime type tests: "is this
  `rt::IAccelStruct` an `AccelStructWrapper`?" (9 sites, including `unwrapResource`) and "is this
  `ICommandList` a `CommandListWrapper`?" (1 site, `executeCommandLists`).

The goal was an NVRHI that never uses `dynamic_cast` or `typeid` and is compiled with RTTI off. The
constraints:

- **Release cost.** `checked_cast` must stay a plain `static_cast` in Release builds.
- **`QueryInterface` adds a reference.** A Debug check that queries an object whose strong count is
  already zero breaks it:
  - `ObjectImpl`: `AddRef` takes the count from 0 to 1, and the matching `Release` takes it back to 0 and
    calls `DestroyObject` a second time.
  - `WeakReferenceSourceImpl`: `AddStrongRef` fails `NVRHI_VERIFY(state == Alive)`, both while the object
    is being destroyed (`Destroyed`) and while it is still being constructed (`NotInitialized`, before
    `MakeNewRCObj` attaches it).

  No `checked_cast` in the tree runs on a dying object today. The destructors and the teardown paths
  (`~Device`, `waitForIdle`, `runGarbageCollection`, the lifetime trackers, `freeBufferMemory`,
  `freeTextureMemory`) were checked and contain none. But the hazard depends on where a cast is written,
  so the check itself must be safe.
- **Headers.** donut and the samples include NVRHI's public headers, including `nvrhi/core`, and are
  compiled with RTTI on. The headers must work with RTTI on or off.

## Decision

**`checked_cast` checks through `QueryInterface`** (`d089ee4`). In Debug builds
`details::QICastMatches(from, to)` (`include/nvrhi/core/types.h`) asks the object for `uuid_of<T>()`.
That is the IID of an interface, or the class ID of an implementation class. The check passes when the
object returns exactly `static_cast<T*>(from)`. Release builds still compile `checked_cast` to a
`static_cast`. Both configurations `static_assert` that the source and the target are `IObject` types.
For any other cast, `unchecked_cast<T>(U)` is an explicit `static_cast`, and every use must say why the
cast is valid. No call site needs it today: all 411 casts are between `IObject` types.

**The check never adds a reference to an object that may be dying.** Two changes to the core make this
possible:

- `QueryInterface(riid, nullptr)` only reports whether `riid` is supported: no pointer, no reference.
  The interface tables already worked this way. `WeakReferenceImpl::QueryInterface`, which returned
  `FE_INVALID_ARGS` for a null `ppv`, now does the same. `IObject::QueryInterface` documents it.
- A liveness probe, `details::QIStrongRefProbeIID`. The base classes that own the reference count
  answer it, and only while the object holds strong references: `ObjectImpl` checks count > 0, and
  `WeakReferenceSourceImpl` checks `!IsExpired()`, that is count > 0 and state `Alive`. The delegating
  base classes forward it to their owner. The probe never returns a pointer. The root layers test it
  only after their table has refused the IID, so a normal query pays nothing. A `QueryInterface` that
  does not reach these base classes never answers the probe, and the check then takes the safe branch.
  Since [ADR 0007](0007-explicit-queryinterface-tables.md) the root layers have no table: the end of the
  class's explicit table (`NVRHI_END_INTERFACE_TABLE()`) tests the probe after a miss and asks the
  non-virtual `NvrhiQIAnswerProbe()` of the base that owns the reference count; a non-delegating table
  never answers it.

`QICastMatches` asks the probe first:

| Probe | Check |
| --- | --- |
| alive | `QueryInterface(iid, &pv)`, `Release()`, compare `pv` with `to` (type and pointer) |
| not known to be alive | `QueryInterface(iid, nullptr)` (type only, no reference touched) |

**Every refcounted implementation class has a class ID that its `QueryInterface` answers.** There are
three new macros in `include/nvrhi/core/foundation.h`:

```cpp
NVRHI_CLASS_CLSID(Texture, "98601210-9058-416e-af5f-f20b8a5e23b5")   // class Texture; + NVRHI_CCLSID
class Texture : public ObjectImpl<ITexture>, public TextureStateExtension
{
public:
    NVRHI_CLASS_INTERFACE_TABLE(Texture)   // uuid traits + table: class ID, then route to ObjectImpl
    ...
};
```

Since [ADR 0007](0007-explicit-queryinterface-tables.md) `NVRHI_CLASS_INTERFACE_TABLE` (and the route it
ended with) is gone. The class writes the traits and an explicit table with every interface and the class
ID:

```cpp
NVRHI_CLASS_CLSID(Texture, "98601210-9058-416e-af5f-f20b8a5e23b5")
class Texture : public ObjectImpl<ITexture>, public TextureStateExtension
{
public:
    NVRHI_DECLARE_UUID_TRAITS(Texture)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(Texture)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::ITexture)
    NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IRHIObject)
    NVRHI_IMPLEMENTS_CLASS(Texture)
    NVRHI_END_INTERFACE_TABLE()
    ...
};
```

- `NVRHI_IMPLEMENTS_CLASS(Class)` is the class ID entry. It returns the class pointer and AddRefs
  through the class. Unlike `NVRHI_IMPLEMENTS_INTERFACE(Class)`, it does not assume that the class's
  `IObject` sits at offset 0. That matters for classes whose first base is a helper, such as Vulkan
  `class Texture : public MemoryResource, public ObjectImpl<ITexture>, ...`.
- 70 classes got a class ID: d3d11 15, d3d12 28, Vulkan 24, validation 3 (`AccelStructWrapper`,
  `CommandListWrapper`, `DeviceWrapper`). The 5 pooled types from ADR 0005 (D3D12 `BufferChunk`,
  `InternalCommandList`, `CommandListInstance`, Vulkan `BufferChunk`, `TrackedCommandBuffer`) keep the
  pattern they already had. That makes 75 class IDs in the backends.
- donut `tools/Gen-Interface.ps1` emits the new macros (since ADR 0007: plain traits and an explicit
  table with the class ID).

**The validation layer's type tests use `queryWrapper<W>(p)`** (`src/validation/validation-backend.h`,
`6cfe4ad`). It does a `QueryInterface` for `W`'s class ID and keeps no reference: `AutoPtr` releases it,
and the caller's reference keeps the wrapper alive. A failed query means "not a wrapper", as a null
`dynamic_cast` did. All 10 sites were converted one for one.

**RTTI is off for every NVRHI target** (`6cfe4ad`). `nvrhi_disable_rtti(target)` in
`cmake/NvrhiCore.cmake` adds `PRIVATE /GR-` (MSVC and clang-cl) or `PRIVATE -fno-rtti` (GCC, Clang).
It applies to `nvrhi_core`, `nvrhi`, `nvrhi_d3d11`, `nvrhi_d3d12` and `nvrhi_vk`. With
`NVRHI_BUILD_SHARED` the backends are part of `nvrhi`, so they get it too. Consumers keep their own
setting. nvrhi's `CMakeLists.txt` drops an explicit `/GR` from `CMAKE_CXX_FLAGS` (older `CMP0117`
behaviour, e.g. a standalone configure with `cmake_minimum_required(VERSION 3.11)`), so `cl` does not
warn D9025 about the override. `/GR` is `cl`'s default, so the test executables keep RTTI.

**Consumers.** Objects created by NVRHI have no RTTI data, so `dynamic_cast` or `typeid` on them in
consumer code is undefined. That includes classes defined in NVRHI headers, such as `DataBlobImpl`.
Consumers use `QueryInterface` with an IID or class ID instead. `dynamic_cast` stays valid on classes
that are defined and instantiated in RTTI-on code, even when they derive from `ObjectImpl`. In donut
(`20d2e3b`):

- Converted: `MediaFileSystem` asked whether its parent is a `NativeFileSystem`. `NativeFileSystem`
  (`IFileSystem : ObjectImpl<IObject>`) now has a class ID, and the test is a `QueryInterface`.
- Kept: the scene graph casts (`SceneGraph.cpp`, `GltfImporter.cpp`, `SceneImporterImpl.cpp`,
  `DrawStrategy.cpp`, `Camera.cpp`, and in etherealsamples `DDGISample.cpp`, `VXGISample.cpp`,
  `VoxelShadingPass.cpp`). These classes derive from `WeakReferenceSourceImpl`, but donut and the samples
  define and create them with RTTI on.
- Kept: `AudioEngine.cpp` (`Xaudio2Effect`), for the same reason. The file also holds an uncommitted
  local fix (ADR 0005, Risks).

## Consequences

### Positive

- NVRHI has no RTTI dependency, and builds and runs with `/GR-`, including as a DLL used by an RTTI-on
  executable.
- One mechanism for type questions: `QueryInterface` with IIDs and class IDs, the same for core,
  backends, validation and consumers.
- The Debug check is stricter than `dynamic_cast`. It also compares pointers, so it catches an object
  that answers the type with a different subobject, such as an aggregated inner object. A
  `dynamic_cast` would cross-cast silently.
- The check is safe on dying objects, as the core tests show.

### Negative

- Each implementation class needs two lines (`NVRHI_CLASS_CLSID` before it, `NVRHI_CLASS_INTERFACE_TABLE`
  inside it; since ADR 0007, the traits and an explicit table with `NVRHI_IMPLEMENTS_CLASS`). A class without them breaks only the Debug build of a `checked_cast` that targets it. On
  MSVC and GCC that is a compile error: `NvrhiUUIDTraits` is not specialized.
- On the Clang path of `types.h`, `uuid_of<Class>()` falls back to an inherited `this_uuid()` when the
  class lacks its own `NVRHI_DECLARE_UUID_TRAITS`. The check then degrades to the parent interface
  instead of failing to compile.
- A Debug `checked_cast` costs one probe and one `QueryInterface` (plus `Release`) instead of one
  `dynamic_cast`.
- When the probe fails, the check tests the type only, not the pointer.
- A class derived from an implementation class must keep the routing rules of ADR 0003. Its table must
  reach the parent's table (`ObjectImpl<Parent>`), or the parent's class ID is lost. Since ADR 0007: its
  table lists `NVRHI_IMPLEMENTS_CLASS(Parent)` or routes to the parent's table with
  `NVRHI_IMPLEMENTS_ROUTE_PARENT(Parent)`. Since the flat base classes (ADR 0007), such a class derives
  from the parent directly (`class Child : public Parent`); `ObjectImpl<Parent>` no longer compiles.

### Risks

- A consumer `dynamic_cast` or `typeid` on an NVRHI object compiles but is undefined. With MSVC it
  reads a missing complete-object locator. GCC typically fails to link, because the typeinfo for NVRHI
  classes is not emitted. Grep for these when porting code.
- Templates that are instantiated in both NVRHI (`/GR-`) and a consumer (`/GR`), such as the
  `QIRootLayer<...>` vtables (since the flat base classes of ADR 0007, the `ObjectImpl<...>` vtables), are
  COMDATs. The linker keeps one copy, possibly without RTTI. This only
  affects `dynamic_cast` while such a base subobject is under construction or destruction. Fully
  constructed objects use the vtable of their most-derived class.
- The probe and the following `AddRef` are not atomic. This only matters if the caller does not hold a
  reference, in which case the raw pointer was already unsafe.

## Alternatives considered

- **Keep `dynamic_cast` in Debug builds only** (`/GR` in Debug, `/GR-` in Release). Rejected:
  - Debug and Release NVRHI would differ in compile options and object layout (vtables with and without
    locators).
  - The validation layer's type tests run in Release builds too, so they need a mechanism without RTTI
    anyway.
  - The goal was no `dynamic_cast` anywhere.
- **A custom type-tag RTTI**, such as a virtual `GetTypeId()`, a static tag per class, or LLVM-style
  `isa`/`classof`. Rejected:
  - It is a second identity system next to IIDs and class IDs.
  - A new virtual on `IObject` changes the vtable of every implementation, including hand-written ones.
  - Per-hierarchy enums do not extend to consumer classes.
- **A non-AddRef `QueryInterface` variant on `IObject`, or an internal interface base on the
  `ObjectImpl` layers.** Rejected. The first changes the `IObject` ABI. The second adds a vptr to every
  object and still needs a way to reach the interface without a reference. The probe IID uses the
  existing vtable slot.
- **A thread-local "suppress AddRef" flag.** Rejected. It costs every `AddRef`, and with
  `NVRHI_BUILD_SHARED` every module has its own copy of an inline `thread_local`.
- **Skip the check when the count is zero, read through `AddRef`/`Release`.** Not possible: reading the
  count that way is exactly what destroys the object.

## References

- nvrhi `lithereal-dev`: `d089ee4` core: QueryInterface-based checked_cast; `6cfe4ad` Replace
  dynamic_cast with QueryInterface; build nvrhi without RTTI.
- donut `ethereal-dev`: `20d2e3b` bump nvrhi (QueryInterface instead of dynamic_cast, no RTTI); vfs: QI
  for NativeFileSystem.
- aggregate root `main`: `0514502` bump donut (nvrhi: QueryInterface instead of dynamic_cast, built
  without RTTI).
- Files: `include/nvrhi/core/types.h` (`QIStrongRefProbeIID`, `QICastMatches`),
  `include/nvrhi/core/foundation.h` (probe in the root layers, `NVRHI_IMPLEMENTS_CLASS`,
  `NVRHI_CLASS_CLSID`, `NVRHI_CLASS_INTERFACE_TABLE`; the latter removed and the probe moved to the table
  end by ADR 0007), `include/nvrhi/common/misc.h` (`checked_cast`,
  `unchecked_cast`), `src/validation/validation-backend.h` (`queryWrapper`), `CMakeLists.txt`,
  `cmake/NvrhiCore.cmake` (`nvrhi_disable_rtti`), `tests/core/test_checked_cast.cpp`,
  `tests/qi-device.cpp`; donut `include/donut/core/vfs/VFS.h`, `src/app/MediaFileSystem.cpp`,
  `tools/Gen-Interface.ps1`.
- Verification:
  - Release and Debug: `nvrhi_test_checked_cast`, `nvrhi_test_auto_ptr`, `nvrhi_test_qi_route`,
    `qi_device_*` and `memory_queries_d3d11/vulkan` pass. `memory_queries_d3d12` fails as before
    (`CreateCommittedResource` 0x80070057 on Windows 10).
  - The standalone `NVRHI_BUILD_SHARED=ON` build passes `qi_device` on all three APIs.
  - Release sample smoke runs are clean, with and without the validation layer.
- Related: [0003](0003-queryinterface-interface-chains.md) (routing rules, superseded by
  [0007](0007-explicit-queryinterface-tables.md)),
  [0005](0005-backends-use-monoptr-and-autoptr.md) (pooled types' class IDs). Plan:
  [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md),
  section 6, follow-ups.
