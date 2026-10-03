# 0005. Backends use `MonoPtr` / `AutoPtr` instead of std smart pointers

- Status: Accepted
- Date: 2026-10-02

## Context

After ADRs [0002](0002-irhiobject-root-interface-and-iids.md) and
[0004](0004-autoptr-replaces-refcountptr.md), public NVRHI objects used the core object model, but the
backends still managed internal objects with std smart pointers:

- `std::unique_ptr` / `std::make_unique` for the state-tracker maps, the per-queue objects, the RTXMU
  managers, uncached shader-table states and the Vulkan upload/scratch managers;
- `std::shared_ptr` / `std::make_shared` for objects shared between a pool and a current user: D3D12
  and Vulkan `BufferChunk`, D3D12 `InternalCommandList` and `CommandListInstance`, and Vulkan
  `TrackedCommandBuffer`.

None of them used a custom deleter, an array form, `std::weak_ptr` or `enable_shared_from_this`. The
aim of the refactor was one ownership vocabulary for the whole tree, owned by `nvrhi::core`.

Converting them exposed three gaps in `MonoPtr` (`include/nvrhi/core/autoptr.h`):

- it had no `operator*`;
- `MakeMono` was `noexcept`, so an exception from `new` or from the constructor called
  `std::terminate`;
- `DefaultDeleter<T>`'s converting constructor tested `std::is_convertible<U, T*>` instead of
  `std::is_convertible<U*, T*>`, so `MonoPtr<Derived>` never converted to `MonoPtr<Base>`.

## Decision

**Unique ownership uses `MonoPtr<T>` / `MakeMono<T>(...)`** (`23ee0df`):

| Member | File |
| --- | --- |
| `CommandListResourceStateTracker::m_TextureStates`, `m_BufferStates` (`std::unordered_map<..., MonoPtr<...>>`) | `src/common/state-tracking.h` |
| D3D12 `Device::m_Queues` (`std::array<MonoPtr<Queue>, ...>`) | `src/d3d12/d3d12-backend.h` |
| D3D12 `Context::rtxMemUtil` (`MonoPtr<rtxmu::DxAccelStructManager>`) | `src/d3d12/d3d12-backend.h` |
| D3D12 `CommandList::m_UncachedShaderTableStates` | `src/d3d12/d3d12-backend.h` |
| Vulkan `Device::m_Queues` | `src/vulkan/vulkan-backend.h` |
| Vulkan `VulkanContext::rtxMemUtil`, `rtxMuResources` | `src/vulkan/vulkan-backend.h` |
| Vulkan `CommandList::m_UncachedShaderTableStates` | `src/vulkan/vulkan-backend.h` |
| Vulkan `CommandList::m_UploadManager`, `m_ScratchManager` | `src/vulkan/vulkan-backend.h` |

The D3D12 upload and scratch managers (`CommandList::m_UploadManager`, `m_DxrScratchManager`) were
already by-value members and did not change.

**Shared ownership uses intrusive reference counting: `AutoPtr<T>` / `MAKE_RC_OBJ_PTR(T, ...)`.** Each
pooled type became a `final` class (or struct) on `ObjectImpl<IObject>` with its own class ID:

| Type | Declared as | Class ID macro | Held in |
| --- | --- | --- | --- |
| D3D12 `BufferChunk` | `class BufferChunk final : public ObjectImpl<IObject>` | `NVRHI_CCLSID` | `UploadManager::m_ChunkPool` (`std::list`), `m_CurrentChunk` |
| D3D12 `InternalCommandList` | `class ... final` | `NVRHI_CCLSID` | `CommandList::m_CommandListPool` (`std::list`), `m_ActiveCommandList` |
| D3D12 `CommandListInstance` | `class ... final` | `NVRHI_CCLSID` | `CommandList::m_Instance`, `CommandListLifetimeTracker::m_CommandListsInFlight` (`std::deque`) |
| Vulkan `BufferChunk` | `struct BufferChunk final : public ObjectImpl<IObject>` | `NVRHI_SCLSID` | `UploadManager::m_ChunkPool`, `m_CurrentChunk` |
| Vulkan `TrackedCommandBuffer` | `class ... final` | `NVRHI_CCLSID` | `TrackedCommandBufferPtr` (now `AutoPtr<TrackedCommandBuffer>`): the queue's pool, lifetime trackers, the recording command list |

The pattern for each type:

```cpp
class BufferChunk;                                            // NVRHI_CCLSID does not declare the class
NVRHI_CCLSID(BufferChunk, "7326d791-307a-43f5-b118-f4455abb2c82")
class BufferChunk final : public ObjectImpl<IObject>
{
public:
    NVRHI_DECLARE_UUID_TRAITS(BufferChunk)
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(BufferChunk)
    NVRHI_IMPLEMENTS_INTERFACE(BufferChunk)                   // answers its own class ID
    NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()                  // everything else: ObjectImpl<IObject>
    ...
};
```

- The class must be declared before `NVRHI_CCLSID`/`NVRHI_SCLSID`. Unlike `NVRHI_IID`, these macros
  do not add a forward declaration; they name `class X` / `struct X` in the trait specialization.
- Instances are created with `MAKE_RC_OBJ` / `MAKE_RC_OBJ_PTR` only. A comment at each declaration
  says so.
- `final` keeps the table complete: nothing can derive from the type and add interfaces that the table
  does not know about.
- These types are internal. They are not `IRHIObject`s and have no public IID.

> Update ([ADR 0007](0007-explicit-queryinterface-tables.md)): `NVRHI_END_INTERFACE_TABLE_ROUTE_PARENT()`
> is gone. The pooled types now write `NVRHI_IMPLEMENTS_INTERFACE(nvrhi::IObject)` (the identity entry)
> and `NVRHI_IMPLEMENTS_CLASS(BufferChunk)`, then `NVRHI_END_INTERFACE_TABLE()`.

**Core additions** (`8f810ce`, made before the backend commit so it builds):

- `MonoPtr<T>::operator*()` returns `std::add_lvalue_reference_t<T>`, as `std::unique_ptr` does.
- `MakeMono` is no longer `noexcept`. Exceptions from `new` and from the constructor propagate.
- `DefaultDeleter<T>` converts from `DefaultDeleter<U>` when `U*` converts to `T*`, so the converting
  move constructor `MonoPtr<Base>(MonoPtr<Derived>&&)` is enabled.
- New tests in `tests/core/test_auto_ptr.cpp`: `Common_MonoPtr.Dereference`,
  `MakeMonoPropagatesExceptions`, `DerivedToBase`, `MoveOnlyInContainers` (MonoPtr in `std::array` and
  `std::unordered_map`), and `Common_RefCntAutoPtr.InternalPooledObject` (the pooled pattern above,
  held in `std::list<AutoPtr<T>>`).

**RTXMU.** The RTXMU members and their `MakeMono` calls are inside `#ifdef NVRHI_WITH_RTXMU`.
`NVRHI_WITH_RTXMU` is OFF by default, and RTXMU is fetched with `FetchContent` (there is no local copy),
so these lines were converted by inspection and have **not been compiled**.

**Result.** No `std::unique_ptr`, `std::shared_ptr`, `std::make_unique`, `std::make_shared` or
`std::weak_ptr` remains in `src/`. The remaining text matches are comments: the explanation at the top
of `include/nvrhi/core/autoptr.h`, one comment in `include/nvrhi/core/foundation.h` ("as in libstdc++'s
shared_ptr"), and one in `tests/core/test_auto_ptr.cpp`. The plan allowed std pointers to stay for
third-party types that need non-intrusive ownership; no such case came up.

## Consequences

### Positive

- One ownership vocabulary in NVRHI: `MonoPtr` for unique, `AutoPtr` for shared, both from
  `nvrhi::core`.
- Pooled objects carry their reference count inside the object. A raw `BufferChunk*` or
  `TrackedCommandBuffer*` can be turned back into an owning `AutoPtr` without a separate control block.
- Allocation of pooled objects goes through the core allocator (`MAKE_RC_OBJ`).
- `MonoPtr` is closer to `std::unique_ptr` (dereference, Derived-to-Base move, throwing factory),
  which also benefits donut.

### Negative

- Each pooled type needs a class ID and a three-line interface table that exist only to satisfy the
  object model.
- `MonoPtr` keeps an implicit `operator pointer()`, which `std::unique_ptr` does not have. Together with
  the `explicit MonoPtr(pointer)` constructor, `MonoPtr<Base>(lvalueMonoPtrDerived)` compiles and
  creates a second owner. donut's `AudioEngine.cpp` had exactly this bug (see Risks).
- The RTXMU paths are unverified until someone builds with `NVRHI_WITH_RTXMU=ON`.

### Risks

- **Double ownership through `operator pointer()`.** donut `src/engine/AudioEngine.cpp`
  (`Xaudio2Implementation::create`) returned `nvrhi::MonoPtr<Engine::Implementation>(result)` with
  `result` an lvalue `MonoPtr<Xaudio2Implementation>`: both pointers own the object, a double free. The
  same code existed before the refactor (donut `a8448f1`, with `donut::MonoPtr`), so it is a latent bug,
  not a regression. The file is always compiled (donut globs `src/engine/*.cpp`), but the code path
  runs only with audio enabled (`DONUT_WITH_AUDIO`, OFF by default). The fix is
  `std::move(result)`, which selects the converting move constructor that `8f810ce` enabled. At the
  time of writing that fix is applied locally in donut and not committed. Removing or making
  `operator pointer()` explicit would catch this class of bug at compile time, but is an API change for
  donut.
- Code that creates a pooled type with `new` would not compile (protected `operator new`), but code
  that holds one by value would compile and break the reference count. The comments at each declaration
  are the only guard.

## Alternatives considered

The plan chose the conversion targets. The other options are recorded for completeness.

- **Keep std smart pointers for internal objects.** Rejected: two ownership vocabularies, and
  `shared_ptr` control blocks for objects that the core model can count intrusively.
- **Keep `std::shared_ptr` for the pooled types, convert only `unique_ptr`.** Rejected for the same
  reason; the pooled types are the main users of shared ownership.
- **Make pooled types `IRHIObject`s with public IIDs.** Rejected: they are internal and must not appear
  in the public interface or in `getNativeObject`.
- **Use `ObjectImpl<IObject>` without a class ID.** Not chosen: `AutoPtr<T>::As`, `CopyTo` and
  `NVRHI_IID_PPV_ARGS` look up `nvrhi::uuid_of<T>()`, so a type without a class ID cannot be used with
  them. The class ID costs one line.

## References

- nvrhi `lithereal-dev`: `8f810ce` core: MonoPtr operator*, throwing MakeMono, Derived->Base
  DefaultDeleter; `23ee0df` Replace std smart pointers with MonoPtr / AutoPtr in backends.
- donut `ethereal-dev`: `236b594` bump nvrhi (std smart pointers -> MonoPtr/AutoPtr).
- aggregate root `main`: `7d26c54` bump donut (nvrhi: std smart pointers -> MonoPtr/AutoPtr).
- Files: `include/nvrhi/core/autoptr.h` (`MonoPtr`, `DefaultDeleter`, `MakeMono`),
  `src/common/state-tracking.h`, `src/d3d12/d3d12-backend.h`, `src/vulkan/vulkan-backend.h`,
  `src/d3d12/d3d12-commandlist.cpp`, `src/d3d12/d3d12-upload.cpp`, `src/vulkan/vulkan-queue.cpp`,
  `src/vulkan/vulkan-upload.cpp`, `tests/core/test_auto_ptr.cpp`; donut `src/engine/AudioEngine.cpp`.
- Plan: [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md),
  step 3d.
