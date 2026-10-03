# 0002. `IRHIObject : IObject` replaces `IResource`; every public interface has an IID

- Status: Accepted
- Date: 2026-10-02

## Context

After [ADR 0001](0001-com-object-model-for-nvrhi.md), NVRHI contained `nvrhi::IObject`, but its own
interfaces still derived from `IResource` (`include/nvrhi/common/resource.h`). `IResource` declared:

- `AddRef()`, `Release()` and `GetRefCount()`, all returning `unsigned long`;
- `getNativeObject(ObjectType)` and `queryMemoryRequirements(MemoryRequirements&)`, both with default
  implementations;
- a protected virtual destructor, and deleted copy and move operations.

Interfaces had no identifiers, so the only way to go from `nvrhi::IDevice*` to `nvrhi::d3d12::IDevice*`
was a cast (`checked_cast`, `dynamic_cast`) that relies on knowing the concrete backend. Objects could
not be passed to donut code that expects an `IObject`.

## Decision

**New root interface.** `include/nvrhi/common/resource.h` now declares:

```cpp
NVRHI_IID(IRHIObject, "3c7ad626-034c-4f05-83fb-e7e3da19f1a0")
struct IRHIObject : IObject
{
    NVRHI_DECLARE_UUID_TRAITS(IRHIObject)   // was NVRHI_DECLARE_UUID_TRAITS_DERIVED(IRHIObject, IObject), see below
    virtual Object getNativeObject(ObjectType objectType) { (void)objectType; return nullptr; }
    virtual bool queryMemoryRequirements(MemoryRequirements& outRequirements) { (void)outRequirements; return false; }
};
typedef AutoPtr<IRHIObject> RHIObjectHandle;
```

- `getNativeObject` (still does not `AddRef` the result) and `queryMemoryRequirements` moved unchanged
  from `IResource` to `IRHIObject`.
- `AddRef`, `Release` and `QueryInterface` come from `IObject` and return `FLONG` / `FRESULT`
  (`int32_t`).
- `GetRefCount()` was dropped. Nothing outside `resource.h` called it. A caller that needs the count can
  use the `AddRef()`/`Release()` return values, as `tests/qi-device.cpp` does.
- `IResource`, `RefCountPtr`, `ResourceHandle` and `RefCounter` were deleted (see
  [ADR 0004](0004-autoptr-replaces-refcountptr.md)).
- `IObject` has no virtual destructor. Objects are destroyed by the `ObjectImpl` machinery, which
  knows the concrete type (see ADR 0004).

**Every reference-counted public interface has an IID.** Each of the 29 public interfaces changed from
`class IX : public IResource` to `struct IX : IRHIObject` (or its real parent), preceded by `NVRHI_IID`
and declaring `NVRHI_DECLARE_UUID_TRAITS_DERIVED(IX, Parent)`, so that `QueryInterface` answered the
whole chain ([ADR 0003](0003-queryinterface-interface-chains.md)). The IIDs are fresh lowercase v4
UUIDs (the literal parser accepts only `a`-`f`).

> Update ([ADR 0007](0007-explicit-queryinterface-tables.md)): the interfaces now declare plain
> `NVRHI_DECLARE_UUID_TRAITS(IX)`; `_DERIVED` and the implicit chains are gone. The parent column below
> is the C++ base. Each implementing class lists the interface and all its ancestors in its explicit
> interface table, for example d3d12 `Device`: `d3d12::IDevice`, `IDevice`, `IRHIObject`. The IIDs did
> not change.

| Interface | IID | Parent | Handle typedef | Header |
| --- | --- | --- | --- | --- |
| `nvrhi::IRHIObject` | `3c7ad626-034c-4f05-83fb-e7e3da19f1a0` | `IObject` | `RHIObjectHandle` | `common/resource.h` |
| `nvrhi::IHeap` | `a64ea90e-4f65-4d35-aa55-ee5d43dd2741` | `IRHIObject` | `HeapHandle` | `nvrhi.h` |
| `nvrhi::ITexture` | `5829563a-41bc-4b8b-a146-e7e780df7e80` | `IRHIObject` | `TextureHandle` | `nvrhi.h` |
| `nvrhi::IStagingTexture` | `1cf135b5-7fee-4c03-806b-c3c90cd08f8a` | `IRHIObject` | `StagingTextureHandle` | `nvrhi.h` |
| `nvrhi::ISamplerFeedbackTexture` | `b8169cc2-f65d-4f3f-9075-54b2cc0e8063` | `IRHIObject` | `SamplerFeedbackTextureHandle` | `nvrhi.h` |
| `nvrhi::IInputLayout` | `ad13ccb3-7c4b-421c-8b3a-4755718addd6` | `IRHIObject` | `InputLayoutHandle` | `nvrhi.h` |
| `nvrhi::IBuffer` | `fa05df63-a065-4801-88b8-31048315896b` | `IRHIObject` | `BufferHandle` | `nvrhi.h` |
| `nvrhi::IShader` | `79abac41-ad5d-49bc-962b-3cb67f98b8a4` | `IRHIObject` | `ShaderHandle` | `nvrhi.h` |
| `nvrhi::IShaderLibrary` | `620bc2fa-911c-4f50-aa87-849d8427fec6` | `IRHIObject` | `ShaderLibraryHandle` | `nvrhi.h` |
| `nvrhi::ISampler` | `2621c0b3-1f47-4bd3-8a9f-b2420f1d477d` | `IRHIObject` | `SamplerHandle` | `nvrhi.h` |
| `nvrhi::IFramebuffer` | `f1f795d1-b929-42e3-a07c-f6600c8eef72` | `IRHIObject` | `FramebufferHandle` | `nvrhi.h` |
| `nvrhi::rt::IOpacityMicromap` | `93d54624-cbb4-457d-a9e9-b20621b39eff` | `IRHIObject` | `rt::OpacityMicromapHandle` | `nvrhi.h` |
| `nvrhi::rt::IAccelStruct` | `43bd2591-ab31-4b51-9721-a1565b398307` | `IRHIObject` | `rt::AccelStructHandle` | `nvrhi.h` |
| `nvrhi::IBindingLayout` | `b33bb739-8c09-4841-89c4-42ea02fbdae6` | `IRHIObject` | `BindingLayoutHandle` | `nvrhi.h` |
| `nvrhi::IBindingSet` | `386643b3-03f0-40de-9067-ebf3f587ea2b` | `IRHIObject` | `BindingSetHandle` | `nvrhi.h` |
| `nvrhi::IDescriptorTable` | `bc6cc093-5745-47f3-a1df-af34c4599987` | `IBindingSet` | `DescriptorTableHandle` | `nvrhi.h` |
| `nvrhi::IGraphicsPipeline` | `23dd2780-ef8a-4b81-a995-bd3bb76446c1` | `IRHIObject` | `GraphicsPipelineHandle` | `nvrhi.h` |
| `nvrhi::IComputePipeline` | `52409d5d-603d-4fe3-8f0a-2d0923b33ad9` | `IRHIObject` | `ComputePipelineHandle` | `nvrhi.h` |
| `nvrhi::IMeshletPipeline` | `67e23a7f-ee13-4b4c-b6a6-d0367a8d46c8` | `IRHIObject` | `MeshletPipelineHandle` | `nvrhi.h` |
| `nvrhi::IEventQuery` | `6440f8e7-e027-45df-883c-e8ab36efa6a2` | `IRHIObject` | `EventQueryHandle` | `nvrhi.h` |
| `nvrhi::ITimerQuery` | `0d0b8f71-95b0-46b6-aefd-00ea0cd51d89` | `IRHIObject` | `TimerQueryHandle` | `nvrhi.h` |
| `nvrhi::rt::IShaderTable` | `238cc44f-409f-48e6-a903-a4df79088385` | `IRHIObject` | `rt::ShaderTableHandle` | `nvrhi.h` |
| `nvrhi::rt::IPipeline` | `98ea5825-5d84-4db9-8ab7-85db0560dfa4` | `IRHIObject` | `rt::PipelineHandle` | `nvrhi.h` |
| `nvrhi::ICommandListLifetimeTracker` | `ced5fbaa-8655-4b2e-83f1-e69f63e2e647` | `IRHIObject` | `CommandListLifetimeTrackerHandle` | `nvrhi.h` |
| `nvrhi::ICommandList` | `17b67ead-70fc-4eb9-b1fc-ade22a25aef5` | `IRHIObject` | `CommandListHandle` | `nvrhi.h` |
| `nvrhi::IDevice` | `055d33c5-95dc-4aab-96fe-39a6829ef8b6` | `IRHIObject` | `DeviceHandle` | `nvrhi.h` |
| `nvrhi::d3d12::IRootSignature` | `0af8f668-e2f0-41ed-bb33-50a9d2cb2d87` | `IRHIObject` | `d3d12::RootSignatureHandle` | `d3d12.h` |
| `nvrhi::d3d12::ICommandList` | `3d064428-ad83-495d-b264-056c7900a1f5` | `nvrhi::ICommandList` | `d3d12::CommandListHandle` | `d3d12.h` |
| `nvrhi::d3d12::IDevice` | `5f3ccc09-1a65-4dc5-a5bf-bac3ed912f6c` | `nvrhi::IDevice` | `d3d12::DeviceHandle` | `d3d12.h` |
| `nvrhi::vulkan::IDevice` | `164f6617-d205-45c4-8f3e-a687a229c270` | `nvrhi::IDevice` | `vulkan::DeviceHandle` | `vulkan.h` |

`d3d11.h` and `validation.h` declare no interfaces of their own. The IIDs are listed here only; the
headers do not carry a separate comment table.

**IID names are namespaced.** `NVRHI_IID(X, "...")` defines `static constexpr nvrhi::GUID IID_X` in the
namespace where it is expanded. The IIDs are therefore `nvrhi::IID_IDevice`,
`nvrhi::d3d12::IID_IDevice`, `nvrhi::vulkan::IID_IDevice`, `nvrhi::rt::IID_IAccelStruct`, and so on.
Generic code should prefer `nvrhi::uuid_of<T>()` or `NVRHI_IID_PPV_ARGS(&ptr)`, which need no name.

**`NVRHI_IID` forward-declares the interface.** On MSVC and GCC, the macro starts with `struct X;` before
it specializes `::NvrhiUUIDTraits<struct X>`. Without that line, the elaborated type specifier
`struct IDevice` inside `namespace nvrhi::d3d12` binds to the already declared `nvrhi::IDevice` of the
enclosing namespace, so `d3d12::IDevice` would get `nvrhi::IDevice`'s trait (a redefinition, or the
wrong IID). The declaration puts the name in the current namespace first (`ce4a071`; tested by
`QueryInterfaceChain.NestedSameNamedInterfaces`, since ADR 0007 `QueryInterfaceTable.NestedSameNamedInterfaces`). The Clang branch does not need it, because there the
IID is found through the member `this_uuid()`.

**`BindingSetItem::resourceHandle` is `IRHIObject*`** (was `IResource*`). The `BindingSetItem`
factory functions assign `ITexture*`, `IBuffer*`, `ISampler*` and `rt::IAccelStruct*` to it. The
`std::hash<BindingSetItem>` specialization still hashes the pointer. Backend code that reads it keeps
using `checked_cast<ITexture*>(binding.resourceHandle)` and similar (for example
`src/d3d12/d3d12-state-tracking.cpp`); replacing those with `QueryInterface` was optional and was not
done.

**Native device object.** `d3d12::Device::getNativeObject(ObjectTypes::Nvrhi_D3D12_Device)` now returns
`static_cast<nvrhi::d3d12::IDevice*>(this)` instead of `this`, so the returned pointer is the interface
pointer regardless of where `ObjectImpl` puts its bases. `tests/qi-device.cpp` checks this, and the
equivalent for `Nvrhi_VK_Device`.

**`std::hash<RefCountPtr<T>>`** at the end of `nvrhi.h` was removed. `autoptr.h` specializes
`std::hash<nvrhi::AutoPtr<T>>`.

**Out of scope.** These types are not reference counted, so they are not `IRHIObject`s and have no IID:

- `nvrhi::IMessageCallback`: implemented and owned by the application, passed as a raw pointer.
- `nvrhi::d3d12::IDescriptorHeap`: owned by the device and returned as a raw pointer by
  `d3d12::IDevice::getDescriptorHeap`.
- `TextureStateExtension` and `BufferStateExtension` (`src/common/state-tracking.h`): internal
  state-tracking mixins inherited next to `ObjectImpl<...>`.
- `vulkan::MemoryResource` (`src/vulkan/vulkan-backend.h`): an internal mixin for objects that own
  device memory.

The validation layer's `DeviceWrapper` implements `nvrhi::IDevice` only (`ObjectImpl<IDevice>`). Asking
it for `d3d12::IID_IDevice` or `vulkan::IID_IDevice` returns `FE_NOINTERFACE`; this is intended and
tested.

## Consequences

### Positive

- Code can ask any NVRHI object for an interface by IID, for example
  `device->QueryInterface(NVRHI_IID_PPV_ARGS(&d3d12Device))`, instead of casting on a guess of the
  backend.
- NVRHI objects are `IObject`s and can be held, queried and passed like any other donut object.
- `queryMemoryRequirements` and `getNativeObject` keep their signatures and defaults; callers only
  change the type name (`IResource` to `IRHIObject`), as `doc/memory-queries.md` now does.
- Most consumer code was source-compatible, because the `*Handle` typedef names did not change.

### Negative

- ABI break for every consumer: the vtable of every interface changed (`QueryInterface` is now the first
  slot, `GetRefCount` and the virtual destructor are gone), and `AddRef`/`Release` return `int32_t`
  instead of `unsigned long`. NVRHI, donut and the samples must be rebuilt together.
- Source break for code that named `IResource`, `ResourceHandle` or `GetRefCount`, or that
  forward-declared an interface as `class` (they are `struct` now; MSVC warns C4099 on a mismatch).
- Each interface declaration carries two extra macro lines.

### Risks

- An interface added later without `NVRHI_IID` and `NVRHI_DECLARE_UUID_TRAITS_DERIVED` still compiles as
  long as nothing asks for its IID, but it will not answer its own `QueryInterface`. Reviewers need to
  check new interfaces for both lines. (Since [ADR 0007](0007-explicit-queryinterface-tables.md): both
  `NVRHI_IID` and plain `NVRHI_DECLARE_UUID_TRAITS`, and every implementing class must list the interface
  and its ancestors in its table; a missing entry is not a compile error.)
- IIDs are part of the ABI from now on and must never be changed or reused.

## Alternatives considered

The plan fixed the `IRHIObject` design and the scope before implementation. The IID naming item is a
departure from the plan; the others are recorded for completeness.

- **Keep `IResource` and add `QueryInterface` to it.** Rejected: it would duplicate `IObject` and keep
  two reference-counting roots in one process.
- **Derive the public interfaces directly from `IObject`, with no `IRHIObject`.** Rejected: there would be
  no type that carries `getNativeObject` and `queryMemoryRequirements`, and no common type for
  `BindingSetItem::resourceHandle` and lifetime-tracking lists.
- **Global IID names that encode the namespace** (`IID_d3d12_IDevice`, as the plan proposed). Rejected
  while implementing: `NVRHI_IID` derives the name from the interface token, and namespacing the
  variable is consistent with the interface names.
- **Give IIDs to `IMessageCallback` and `IDescriptorHeap` as well.** Rejected: they are not reference
  counted, and turning them into `IObject`s would change their ownership rules for applications.

## References

- nvrhi `lithereal-dev`: `a500ab7` Re-base the NVRHI interfaces on nvrhi::IObject;
  `ce4a071` core: interface chains for QueryInterface (`NVRHI_IID` forward declaration);
  `54e011f` tests: QueryInterface on real backend objects; `ed06c3c` doc, natvis.
- Files: `include/nvrhi/common/resource.h`, `include/nvrhi/nvrhi.h`, `include/nvrhi/d3d12.h`,
  `include/nvrhi/vulkan.h`, `include/nvrhi/core/types.h`, `tests/qi-device.cpp`,
  `doc/ProgrammingGuide.md`, `doc/memory-queries.md`.
- Plan: [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md),
  steps 3a-3c.
