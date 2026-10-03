# 0004. `AutoPtr<T>` replaces `RefCountPtr<T>`, including for native COM objects

- Status: Accepted
- Date: 2026-10-02

## Context

NVRHI had its own intrusive smart pointer, `RefCountPtr<T>` ("mostly a copy of `Microsoft::WRL::ComPtr`",
in `include/nvrhi/common/resource.h`), and the implementation base `RefCounter<T>`, which implemented
`AddRef`/`Release`/`GetRefCount` and did `delete this` at zero. `RefCountPtr` held both NVRHI interfaces
(`typedef RefCountPtr<ITexture> TextureHandle`) and native COM interfaces (`RefCountPtr<ID3D12Resource>`,
`RefCountPtr<IDXGISwapChain1>`), in NVRHI and in donut.

`nvrhi::core` ([ADR 0001](0001-com-object-model-for-nvrhi.md)) brings `AutoPtr<T>`, which does the
same job for any type with `AddRef`/`Release`, plus `As`, `CopyTo` and weak-reference support, and
`ObjectImpl<...>`/`MAKE_RC_OBJ` for implementations. Keeping both pointer types would mean two
spellings for the same thing and conversions at every boundary.

## Decision

**One smart pointer: `nvrhi::AutoPtr<T>`.** It holds NVRHI interfaces, backend implementation classes
(for example `AutoPtr<RootSignature>`, `AutoPtr<Buffer>`) and native D3D/DXGI COM interfaces
(`AutoPtr<ID3D12Resource>`, `AutoPtr<IDXGIAdapter>`). The name stays `AutoPtr`: renaming it to `ComPtr`
was considered and declined when the plan was approved, so there is no `nvrhi::ComPtr`, and none is
planned. The `*Handle` typedefs keep their names and are now `AutoPtr<IX>`.

**Deleted.** `RefCountPtr<T>`, `RefCounter<T>`, `IResource` and `ResourceHandle` were removed from
`resource.h` in `a500ab7`, the same commit that converted every user. `IResource` is replaced by
`IRHIObject` ([ADR 0002](0002-irhiobject-root-interface-and-iids.md)).

**Implementations.** Backend classes derive from `ObjectImpl<IX>` instead of `RefCounter<IX>`. Objects
are created and adopted as follows:

| Before | After |
| --- | --- |
| `Texture* t = new Texture(args...);` | `Texture* t = MAKE_RC_OBJ(Texture, args...);` |
| `return TextureHandle::Create(t);` | `return TakeOver(t);` |
| `delete t;` on an error path | `t->Release();` |
| `XHandle::Create(new X(args...))` | `TakeOver(MAKE_RC_OBJ(X, args...))` or `MAKE_RC_OBJ_PTR(X, args...)` |
| `RefCountPtr<ID3D12X>::Create(raw)` | `TakeOver(raw)` |

`MAKE_RC_OBJ` returns a raw pointer that already holds one reference. `TakeOver` adopts that reference
without adding one, like the old `Create`. `ObjectImpl` makes `operator new`/`operator delete`
protected (through `UserAllocated`), so `new X` and `delete x` outside `MakeNewRCObj` no longer compile.
`IObject` has no virtual destructor: the object wrapper that `MAKE_RC_OBJ` attaches records the
concrete type and allocator, and destroys through them when the count reaches zero.

**Native COM interfaces and `IID_PPV_ARGS`.** `AutoPtr<T>` only needs `AddRef`/`Release` (and
`QueryInterface` for `As`/`CopyTo`), so it holds `IUnknown`-derived interfaces directly. Its
`operator&` returns `nvrhi::details::AutoPtrRef<AutoPtr<T>>`, which converts to `T**` and `void**`
(both through `ReleaseAndGetAddressOf()`). The Windows SDK macro
`IID_PPV_ARGS(pp)` expands to `__uuidof(**(pp)), IID_PPV_ARGS_Helper(pp)`:

- `**(&p)` works because `AutoPtrRef::operator*()` returns `T*`.
- `autoptr.h` declares a global overload
  `template <typename T> void** IID_PPV_ARGS_Helper(nvrhi::details::AutoPtrRef<T> pp)`, so the helper
  accepts the `AutoPtrRef`.

So `D3D12CreateDevice(..., IID_PPV_ARGS(&p))` and `factory->EnumAdapters(i, &adapter)` work unchanged.

**The single-argument `QueryInterface(&p)` had to change.** `IUnknown` has a template overload
`template <class Q> HRESULT QueryInterface(Q** pp)`. `RefCountPtr::operator&` returned `T**`, so `Q`
was deduced. `AutoPtr::operator&` returns an `AutoPtrRef`, and template argument deduction does not
consider its conversion operators, so the call no longer compiles. Every such call became
`QueryInterface(IID_PPV_ARGS(&p))`: the `ID3D12Device` queries for `device2`/`device5`/`device8`/
`device10`/`devicePreview` in `src/d3d12/d3d12-device.cpp` (`a500ab7`) and the `ID3D12InfoQueue` query in
donut `src/app/dx12/DeviceManager_DX12.cpp` (`3544c7b`).

**Consumers.** donut `3544c7b` changed `DeviceManager_DX11`, `DeviceManager_DX12` and
`StreamlineIntegration.h` from `nvrhi::RefCountPtr<IDXGI*/ID3D1x*>` to `nvrhi::AutoPtr<...>`.
etherealsamples `1ad7a40` did the same in the Asteroids benchmark (`benchmark/Asteroids/nvrhi`).

**Debugger and docs** (`ed06c3c`):

- `tools/nvrhi.natvis`: the visualizer for `nvrhi::RefCountPtr<*>` now matches `nvrhi::AutoPtr<*>`.
  `AutoPtr`'s member is also called `ptr_`, so the body did not change.
- `doc/ProgrammingGuide.md`, section "Resources": `IRHIObject`/`IObject` instead of `IResource`,
  `AutoPtr<T>` instead of `RefCountPtr<T>`, `TakeOver`, the per-interface IIDs with a
  `NVRHI_IID_PPV_ARGS` example, and `ObjectImpl`/`MAKE_RC_OBJ` for implementations.
- `doc/memory-queries.md`: `IResource::queryMemoryRequirements` becomes
  `IRHIObject::queryMemoryRequirements`.
- `doc/Tutorial.md` named none of the removed types and was not changed.

**Exception: Vulkan `Queue::m_LifetimeTracker`.** `nvrhi::vulkan::Queue` (`src/vulkan/vulkan-backend.h`)
keeps its default `CommandListLifetimeTracker` as a by-value member, not created with `MAKE_RC_OBJ`.
The comment at the member explains why: it is used only through raw pointers
(`lifetimeTracker = &m_LifetimeTracker` in `src/vulkan/vulkan-queue.cpp`) and never handed out as a
handle, so its reference count stays at its initial value and never reaches zero. Changing it to a
heap object would add an allocation and a handle with no benefit.

## Consequences

### Positive

- One smart pointer for NVRHI objects, implementation classes and native COM objects.
- Objects can only be created through `MAKE_RC_OBJ`, so allocation always goes through the core
  allocator and destruction always goes through the recorded concrete type.
- `AutoPtr` adds `As<U>()` and `CopyTo` through `QueryInterface`, and `WeakPtr` for objects that opt in
  to weak references.
- Existing `*Handle` code and most `IID_PPV_ARGS` code compiled unchanged.

### Negative

- `QueryInterface(&p)` (the single-argument template) does not compile with `AutoPtr`; callers must
  write `QueryInterface(IID_PPV_ARGS(&p))`.
- `Handle::Create` is gone. Out-of-tree code that adopted raw pointers with it must use `TakeOver`.
- Out-of-tree code that subclassed `RefCounter<T>` or created objects with `new` must move to
  `ObjectImpl<T>` and `MAKE_RC_OBJ`.

### Risks

- `&ptr` on an `AutoPtr` releases the held object before returning the address (the same as
  `RefCountPtr` and WRL `ComPtr`). `AutoPtrRef` also converts to `AutoPtr<T>*`, which clears the
  pointer. Use `GetAddressOf()` or `std::addressof(ptr)` when the current value must be kept.
- `m_LifetimeTracker` relies on balanced `AddRef`/`Release`. If anyone wraps it with `TakeOver`, or
  releases it once more than it was added, the count reaches zero and `DestroyObject` runs on an object
  that was never attached to a wrapper.

## Alternatives considered

The plan settled the name and the scope. Keeping both pointer types is recorded for completeness.

- **Rename `AutoPtr` to `ComPtr`**, or add `ComPtr` as an alias. Declined: the name `AutoPtr` was
  already used throughout donut, and an alias would give two names for one type.
- **Keep `RefCountPtr` as an alias of `AutoPtr`.** Rejected for the same reason as the donut
  forwarding shims in ADR 0001: the old spelling would never go away.
- **Keep `RefCountPtr` for native COM objects and use `AutoPtr` only for `IObject`s.** Rejected: two
  pointer types with the same behavior, and the native ones would still need a home after
  `resource.h` was emptied.
- **Use WRL `Microsoft::WRL::ComPtr` for native objects.** Rejected: Windows only (the Vulkan backend
  builds elsewhere), and it would add a third pointer type.

## References

- nvrhi `lithereal-dev`: `a500ab7` Re-base the NVRHI interfaces on nvrhi::IObject; `ed06c3c` doc,
  natvis: AutoPtr / IObject / IRHIObject.
- donut `ethereal-dev`: `3544c7b` app: nvrhi::AutoPtr for native D3D/DXGI interfaces; bump nvrhi.
- etherealsamples `main`: `1ad7a40` asteroids_nvrhi: nvrhi::AutoPtr for native D3D/DXGI interfaces.
- aggregate root `main`: `e661df1` bump donut, etherealsamples (nvrhi interfaces on nvrhi::IObject).
- Files: `include/nvrhi/core/autoptr.h` (`AutoPtrRef`, `TakeOver`, `IID_PPV_ARGS_Helper`),
  `include/nvrhi/core/foundation.h` (`UserAllocated`, `MakeNewRCObj`, `MAKE_RC_OBJ`),
  `include/nvrhi/common/resource.h`, `src/vulkan/vulkan-backend.h`, `src/vulkan/vulkan-queue.cpp`,
  `tools/nvrhi.natvis`, `doc/ProgrammingGuide.md`, `doc/memory-queries.md`.
- Plan: [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md),
  steps 3a, 3c and 3e.
