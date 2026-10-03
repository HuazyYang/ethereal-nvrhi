# 0007 appendix. QueryInterface table audit

Part of [ADR 0007](0007-explicit-queryinterface-tables.md). Date: 2026-10-02. First run at nvrhi `5922252`,
donut `b2f615c`, etherealsamples `2d53121`; re-run for the per-class check at nvrhi `9990dba`, donut
`3a36fb8`, etherealsamples `54a4610` (the tables below are from the re-run).

## Method

A script scanned every C++ source of nvrhi (`include`, `src`, `tests`), donut (`include`, `src`, `tests`)
and etherealsamples (`src`, `benchmark`), with comments, strings and `#if 0` blocks removed, and tracked
namespaces and class scopes. For each class whose bases include `ObjectImpl`,
`WeakReferenceSourceImpl` or a delegating base:

- **Expected:** `IObject`, every interface (a type declared with `NVRHI_IID`) reachable through its
  bases and the `ObjectImpl<...>` arguments, and the class IDs (`NVRHI_CLASS_CLSID` / `NVRHI_CCLSID` /
  `NVRHI_SCLSID`) of the class and of its implementation ancestors. A template is checked through the
  classes derived from it (its arguments substituted).
- **Answered:** the IIDs of the class's own table, or of the table it inherits from its nearest base with
  one: `NVRHI_IMPLEMENTS_INTERFACE` / `_AS` / `_CLASS` entries, `NVRHI_IMPLEMENTS_ROUTE_PARENT(Base)`
  followed into Base's table, and `NVRHI_IMPLEMENTS_ROUTE_MEMBER(m)` followed into the member type's
  non-delegating table (reported as aggregation, beyond the inheritance graph). `IObject` is answered by
  the walker; a non-delegating table (ND) refuses it.
- **Result:** OK when answered equals expected (plus aggregated interfaces), and the first entry is an
  offset entry.

The runtime counterpart is `tests/qi-device.cpp`: on D3D11, D3D12 and Vulkan, directly and through the
validation layer, each created object answers exactly its interfaces and class ID and refuses every
other public IID and class ID.

**Declared** (the per-class check): "own" when the class's body has a table macro
(`NVRHI_BEGIN_..._INLINE`, or `NVRHI_DECLARE_..._INTERFACE_TABLE()` for an out-of-line table), "inherit
(declared)" when it reuses its base's table and says so with `NVRHI_INHERIT_INTERFACE_TABLE()`, "NOT
DECLARED" when it reuses a base's table silently (`MakeNewRCObj` rejects such a class; only test fixtures
that check exactly that are left), "own (QueryInterface declared by hand)" for an out-of-line table whose
class declares `QueryInterface` itself instead of using `NVRHI_DECLARE_INTERFACE_TABLE()` (accepted, see
the ADR).

Two fixes to the script for the re-run: classes are told apart per program (each sample directory and
each test file is its own program), so same-named classes in different samples are no longer merged into
one entry; and directories named `nvrhi` are skipped only inside donut (where `donut/nvrhi` is the nvrhi
repository), so the Asteroids benchmark's `benchmark/Asteroids/nvrhi` is scanned. The first run had missed
four sample classes this way: `UIPass` (DDGISample; merged with VXGISample's `UIPass`), `UIRenderer`
(aftermath; merged with work_graphs' `UIRenderer`), `VariableRateShading` (rt_reflections; merged with
variable_shading's) and `AsteroidsRenderer` (skipped). All four reuse `IRenderPass`'s table correctly and
now declare it; the build found `UIPass` and `AsteroidsRenderer` through the new check, the re-run found
all four.

## Summary

| Repository | Classes | Own table | Inherit (declared) | Not declared | Result |
| --- | --- | --- | --- | --- | --- |
| nvrhi (core and backends) | 79 | 79 | 0 | 0 | all OK |
| nvrhi (test fixtures) | 59 | 40 (incl. 2 with a hand-declared `QueryInterface`, 1 template) | 4 (incl. 1 template) | 3 (incl. 1 template; on purpose, static_assert only) | 43 OK (incl. the 2 non-template not-declared ones); 3 templates; 3 abstract on purpose (no table), 1 refuses unlisted ancestors on purpose, 8 hand-written `QueryInterface` stubs, 1 class derived from one (on purpose, static_assert only) |
| donut | 91 | 47 | 44 (incl. `StereoView<T>`) | 0 | 90 OK; 1 template (`StereoView<T>`, reuses `ICompositeView`'s table through `IView`) |
| etherealsamples | 52 | 19 (incl. `DeviceChild<T>`) | 33 | 0 | 51 OK; 1 template (`DeviceChild<T>`, checked through its 5 derived classes) |

No production class answers fewer or more IIDs than its inheritance graph plus its class ID(s), before or
after the per-class check: all 77 classes that reused a base's table add no interface and no class ID, so
all of them got `NVRHI_INHERIT_INTERFACE_TABLE()` and none needed a table of its own. Every production
class is "own" or "inherit (declared)".

**Flat base classes** (nvrhi `e8a7951`, ADR 0007 "Flat base classes"): the pass-through layer is gone, so no
class has an `ObjectImpl<...>` argument that is itself an implementation class (the base classes reject it
with a `static_assert`). No production class had one. The six test fixtures that did (`QIRouteTest::Bar`,
`BarListed`, `Leaf`, `WeakBar`, `InnerEx`, `QIUser::App::XLeaf`) now derive from their implementation
directly; their tables and the IIDs they answer are unchanged, so their rows below stand. One fixture was
added, `CheckedCastTest::DerivedFoo` (a class derived from `FooImpl` directly).

Abbreviations: `nvrhi::` and `donut::` are left out; "ND" is the non-delegating table.

### nvrhi (production code, 79 classes)

| Class | File | Declared | Table | Answers (besides IObject) | Result |
| --- | --- | --- | --- | --- | --- |
| `DataBlobImpl` | `include/nvrhi/core/datablob.h` | own | own | IDataBlob, DataBlobImpl | OK |
| `ProxyDataBlobImpl` | `include/nvrhi/core/datablob.h` | own | own | IDataBlob, ProxyDataBlobImpl | OK |
| `ProxyRefDataBlobImpl` | `include/nvrhi/core/datablob.h` | own | own | IDataBlob, ProxyRefDataBlobImpl | OK |
| `StringDataBlobImpl` | `include/nvrhi/core/datablob.h` | own | own | IDataBlob, StringDataBlobImpl | OK |
| `d3d11::BindingLayout` | `src/d3d11/d3d11-backend.h` | own | own | IBindingLayout, IRHIObject, d3d11::BindingLayout | OK |
| `d3d11::BindingSet` | `src/d3d11/d3d11-backend.h` | own | own | IBindingSet, IRHIObject, d3d11::BindingSet | OK |
| `d3d11::Buffer` | `src/d3d11/d3d11-backend.h` | own | own | IBuffer, IRHIObject, d3d11::Buffer | OK |
| `d3d11::CommandList` | `src/d3d11/d3d11-backend.h` | own | own | ICommandList, IRHIObject, d3d11::CommandList | OK |
| `d3d11::ComputePipeline` | `src/d3d11/d3d11-backend.h` | own | own | IComputePipeline, IRHIObject, d3d11::ComputePipeline | OK |
| `d3d11::Device` | `src/d3d11/d3d11-backend.h` | own | own | IDevice, IRHIObject, d3d11::Device | OK |
| `d3d11::EventQuery` | `src/d3d11/d3d11-backend.h` | own | own | IEventQuery, IRHIObject, d3d11::EventQuery | OK |
| `d3d11::Framebuffer` | `src/d3d11/d3d11-backend.h` | own | own | IFramebuffer, IRHIObject, d3d11::Framebuffer | OK |
| `d3d11::GraphicsPipeline` | `src/d3d11/d3d11-backend.h` | own | own | IGraphicsPipeline, IRHIObject, d3d11::GraphicsPipeline | OK |
| `d3d11::InputLayout` | `src/d3d11/d3d11-backend.h` | own | own | IInputLayout, IRHIObject, d3d11::InputLayout | OK |
| `d3d11::Sampler` | `src/d3d11/d3d11-backend.h` | own | own | ISampler, IRHIObject, d3d11::Sampler | OK |
| `d3d11::Shader` | `src/d3d11/d3d11-backend.h` | own | own | IShader, IRHIObject, d3d11::Shader | OK |
| `d3d11::StagingTexture` | `src/d3d11/d3d11-backend.h` | own | own | IStagingTexture, IRHIObject, d3d11::StagingTexture | OK |
| `d3d11::Texture` | `src/d3d11/d3d11-backend.h` | own | own | ITexture, IRHIObject, d3d11::Texture | OK |
| `d3d11::TimerQuery` | `src/d3d11/d3d11-backend.h` | own | own | ITimerQuery, IRHIObject, d3d11::TimerQuery | OK |
| `d3d12::AccelStruct` | `src/d3d12/d3d12-backend.h` | own | own | rt::IAccelStruct, IRHIObject, d3d12::AccelStruct | OK |
| `d3d12::BindingLayout` | `src/d3d12/d3d12-backend.h` | own | own | IBindingLayout, IRHIObject, d3d12::BindingLayout | OK |
| `d3d12::BindingSet` | `src/d3d12/d3d12-backend.h` | own | own | IBindingSet, IRHIObject, d3d12::BindingSet | OK |
| `d3d12::BindlessLayout` | `src/d3d12/d3d12-backend.h` | own | own | IBindingLayout, IRHIObject, d3d12::BindlessLayout | OK |
| `d3d12::Buffer` | `src/d3d12/d3d12-backend.h` | own | own | IBuffer, IRHIObject, d3d12::Buffer | OK |
| `d3d12::BufferChunk` | `src/d3d12/d3d12-backend.h` | own | own | d3d12::BufferChunk | OK |
| `d3d12::CommandList` | `src/d3d12/d3d12-backend.h` | own | own | d3d12::ICommandList, ICommandList, IRHIObject, d3d12::CommandList | OK |
| `d3d12::CommandListInstance` | `src/d3d12/d3d12-backend.h` | own | own | d3d12::CommandListInstance | OK |
| `d3d12::CommandListLifetimeTracker` | `src/d3d12/d3d12-backend.h` | own | own | ICommandListLifetimeTracker, IRHIObject, d3d12::CommandListLifetimeTracker | OK |
| `d3d12::ComputePipeline` | `src/d3d12/d3d12-backend.h` | own | own | IComputePipeline, IRHIObject, d3d12::ComputePipeline | OK |
| `d3d12::DescriptorTable` | `src/d3d12/d3d12-backend.h` | own | own | IDescriptorTable, IBindingSet, IRHIObject, d3d12::DescriptorTable | OK |
| `d3d12::Device` | `src/d3d12/d3d12-backend.h` | own | own | d3d12::IDevice, IDevice, IRHIObject, d3d12::Device | OK |
| `d3d12::EventQuery` | `src/d3d12/d3d12-backend.h` | own | own | IEventQuery, IRHIObject, d3d12::EventQuery | OK |
| `d3d12::Framebuffer` | `src/d3d12/d3d12-backend.h` | own | own | IFramebuffer, IRHIObject, d3d12::Framebuffer | OK |
| `d3d12::GraphicsPipeline` | `src/d3d12/d3d12-backend.h` | own | own | IGraphicsPipeline, IRHIObject, d3d12::GraphicsPipeline | OK |
| `d3d12::Heap` | `src/d3d12/d3d12-backend.h` | own | own | IHeap, IRHIObject, d3d12::Heap | OK |
| `d3d12::InputLayout` | `src/d3d12/d3d12-backend.h` | own | own | IInputLayout, IRHIObject, d3d12::InputLayout | OK |
| `d3d12::InternalCommandList` | `src/d3d12/d3d12-backend.h` | own | own | d3d12::InternalCommandList | OK |
| `d3d12::MeshletPipeline` | `src/d3d12/d3d12-backend.h` | own | own | IMeshletPipeline, IRHIObject, d3d12::MeshletPipeline | OK |
| `d3d12::OpacityMicromap` | `src/d3d12/d3d12-backend.h` | own | own | rt::IOpacityMicromap, IRHIObject, d3d12::OpacityMicromap | OK |
| `d3d12::RayTracingPipeline` | `src/d3d12/d3d12-backend.h` | own | own | rt::IPipeline, IRHIObject, d3d12::RayTracingPipeline | OK |
| `d3d12::RootSignature` | `src/d3d12/d3d12-backend.h` | own | own | d3d12::IRootSignature, IRHIObject, d3d12::RootSignature | OK |
| `d3d12::Sampler` | `src/d3d12/d3d12-backend.h` | own | own | ISampler, IRHIObject, d3d12::Sampler | OK |
| `d3d12::SamplerFeedbackTexture` | `src/d3d12/d3d12-backend.h` | own | own | ISamplerFeedbackTexture, IRHIObject, d3d12::SamplerFeedbackTexture | OK |
| `d3d12::Shader` | `src/d3d12/d3d12-backend.h` | own | own | IShader, IRHIObject, d3d12::Shader | OK |
| `d3d12::ShaderLibrary` | `src/d3d12/d3d12-backend.h` | own | own | IShaderLibrary, IRHIObject, d3d12::ShaderLibrary | OK |
| `d3d12::ShaderLibraryEntry` | `src/d3d12/d3d12-backend.h` | own | own | IShader, IRHIObject, d3d12::ShaderLibraryEntry | OK |
| `d3d12::ShaderTable` | `src/d3d12/d3d12-backend.h` | own | own | rt::IShaderTable, IRHIObject, d3d12::ShaderTable | OK |
| `d3d12::StagingTexture` | `src/d3d12/d3d12-backend.h` | own | own | IStagingTexture, IRHIObject, d3d12::StagingTexture | OK |
| `d3d12::Texture` | `src/d3d12/d3d12-backend.h` | own | own | ITexture, IRHIObject, d3d12::Texture | OK |
| `d3d12::TimerQuery` | `src/d3d12/d3d12-backend.h` | own | own | ITimerQuery, IRHIObject, d3d12::TimerQuery | OK |
| `validation::AccelStructWrapper` | `src/validation/validation-backend.h` | own | own | rt::IAccelStruct, IRHIObject, validation::AccelStructWrapper | OK |
| `validation::CommandListWrapper` | `src/validation/validation-backend.h` | own | own | ICommandList, IRHIObject, validation::CommandListWrapper | OK |
| `validation::DeviceWrapper` | `src/validation/validation-backend.h` | own | own | IDevice, IRHIObject, validation::DeviceWrapper | OK |
| `vulkan::AccelStruct` | `src/vulkan/vulkan-backend.h` | own | own | rt::IAccelStruct, IRHIObject, vulkan::AccelStruct | OK |
| `vulkan::BindingLayout` | `src/vulkan/vulkan-backend.h` | own | own | IBindingLayout, IRHIObject, vulkan::BindingLayout | OK |
| `vulkan::BindingSet` | `src/vulkan/vulkan-backend.h` | own | own | IBindingSet, IRHIObject, vulkan::BindingSet | OK |
| `vulkan::Buffer` | `src/vulkan/vulkan-backend.h` | own | own | IBuffer, IRHIObject, vulkan::Buffer | OK |
| `vulkan::BufferChunk` | `src/vulkan/vulkan-backend.h` | own | own | vulkan::BufferChunk | OK |
| `vulkan::CommandList` | `src/vulkan/vulkan-backend.h` | own | own | ICommandList, IRHIObject, vulkan::CommandList | OK |
| `vulkan::CommandListLifetimeTracker` | `src/vulkan/vulkan-backend.h` | own | own | ICommandListLifetimeTracker, IRHIObject, vulkan::CommandListLifetimeTracker | OK |
| `vulkan::ComputePipeline` | `src/vulkan/vulkan-backend.h` | own | own | IComputePipeline, IRHIObject, vulkan::ComputePipeline | OK |
| `vulkan::DescriptorTable` | `src/vulkan/vulkan-backend.h` | own | own | IDescriptorTable, IBindingSet, IRHIObject, vulkan::DescriptorTable | OK |
| `vulkan::Device` | `src/vulkan/vulkan-backend.h` | own | own | vulkan::IDevice, IDevice, IRHIObject, vulkan::Device | OK |
| `vulkan::EventQuery` | `src/vulkan/vulkan-backend.h` | own | own | IEventQuery, IRHIObject, vulkan::EventQuery | OK |
| `vulkan::Framebuffer` | `src/vulkan/vulkan-backend.h` | own | own | IFramebuffer, IRHIObject, vulkan::Framebuffer | OK |
| `vulkan::GraphicsPipeline` | `src/vulkan/vulkan-backend.h` | own | own | IGraphicsPipeline, IRHIObject, vulkan::GraphicsPipeline | OK |
| `vulkan::Heap` | `src/vulkan/vulkan-backend.h` | own | own | IHeap, IRHIObject, vulkan::Heap | OK |
| `vulkan::InputLayout` | `src/vulkan/vulkan-backend.h` | own | own | IInputLayout, IRHIObject, vulkan::InputLayout | OK |
| `vulkan::MeshletPipeline` | `src/vulkan/vulkan-backend.h` | own | own | IMeshletPipeline, IRHIObject, vulkan::MeshletPipeline | OK |
| `vulkan::OpacityMicromap` | `src/vulkan/vulkan-backend.h` | own | own | rt::IOpacityMicromap, IRHIObject, vulkan::OpacityMicromap | OK |
| `vulkan::RayTracingPipeline` | `src/vulkan/vulkan-backend.h` | own | own | rt::IPipeline, IRHIObject, vulkan::RayTracingPipeline | OK |
| `vulkan::Sampler` | `src/vulkan/vulkan-backend.h` | own | own | ISampler, IRHIObject, vulkan::Sampler | OK |
| `vulkan::Shader` | `src/vulkan/vulkan-backend.h` | own | own | IShader, IRHIObject, vulkan::Shader | OK |
| `vulkan::ShaderLibrary` | `src/vulkan/vulkan-backend.h` | own | own | IShaderLibrary, IRHIObject, vulkan::ShaderLibrary | OK |
| `vulkan::ShaderTable` | `src/vulkan/vulkan-backend.h` | own | own | rt::IShaderTable, IRHIObject, vulkan::ShaderTable | OK |
| `vulkan::StagingTexture` | `src/vulkan/vulkan-backend.h` | own | own | IStagingTexture, IRHIObject, vulkan::StagingTexture | OK |
| `vulkan::Texture` | `src/vulkan/vulkan-backend.h` | own | own | ITexture, IRHIObject, vulkan::Texture | OK |
| `vulkan::TimerQuery` | `src/vulkan/vulkan-backend.h` | own | own | ITimerQuery, IRHIObject, vulkan::TimerQuery | OK |
| `vulkan::TrackedCommandBuffer` | `src/vulkan/vulkan-backend.h` | own | own | vulkan::TrackedCommandBuffer | OK |

### nvrhi (test fixtures, 59 classes)

| Class | File | Declared | Table | Answers (besides IObject) | Result |
| --- | --- | --- | --- | --- | --- |
| `Test::DelegatingObj` | `tests/core/test_auto_ptr.cpp` | own | own | ND: Test::DelegatingObj | OK |
| `Test::DerivedObject` | `tests/core/test_auto_ptr.cpp` | own (QueryInterface declared by hand) | own (route parent Object) | IWeakReferenceSource, Test::DerivedObject, Test::Object | OK |
| `Test::ExceptionTest1` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `Test::ExceptionTest2` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `Test::ExceptionTest3` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `Test::Object` | `tests/core/test_auto_ptr.cpp` | own (QueryInterface declared by hand) | own | IWeakReferenceSource, Test::Object | OK |
| `Test::OwnerObject` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `Test::OwnerObject::ExceptionTest4` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `Test::OwnerTest` | `tests/core/test_auto_ptr.cpp` | own | own (route member Obj (DelegatingObj)) | IWeakReferenceSource, Test::DelegatingObj | OK |
| `Test::PooledChunk` | `tests/core/test_auto_ptr.cpp` | own | own | Test::PooledChunk | OK |
| `Test::SelfRefTest` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `Test::StrongObject` | `tests/core/test_auto_ptr.cpp` | own | own | (IObject only) | OK |
| `Test::TestObject` | `tests/core/test_auto_ptr.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `CheckedCastTest::DerivedFoo` | `tests/core/test_checked_cast.cpp` | own | own (route parent FooImpl) | CheckedCastTest::IFoo, CheckedCastTest::DerivedFoo, CheckedCastTest::FooImpl | OK |
| `CheckedCastTest::FooBar` | `tests/core/test_checked_cast.cpp` | own | own | CheckedCastTest::IFoo, CheckedCastTest::IBar, CheckedCastTest::FooBar | OK |
| `CheckedCastTest::FooImpl` | `tests/core/test_checked_cast.cpp` | own | own | CheckedCastTest::IFoo, CheckedCastTest::FooImpl | OK |
| `CheckedCastTest::InnerFoo` | `tests/core/test_checked_cast.cpp` | own | own | ND: CheckedCastTest::IFoo | OK |
| `CheckedCastTest::OtherFoo` | `tests/core/test_checked_cast.cpp` | own | own | CheckedCastTest::IFoo, CheckedCastTest::OtherFoo | OK |
| `CheckedCastTest::OuterBar` | `tests/core/test_checked_cast.cpp` | own | own (route member m_pInner (InnerFoo)) | CheckedCastTest::IBar, CheckedCastTest::IFoo | OK |
| `CheckedCastTest::SelfCastInDtor` | `tests/core/test_checked_cast.cpp` | own | own | CheckedCastTest::IFoo, CheckedCastTest::SelfCastInDtor | OK |
| `CheckedCastTest::WeakSelfCast` | `tests/core/test_checked_cast.cpp` | own | own | IWeakReferenceSource, CheckedCastTest::IFoo, CheckedCastTest::WeakSelfCast | OK |
| `QIDerivedTest::DevImpl` | `tests/core/test_qi_route.cpp` | own | own | QIDerivedTest::backend::IDev, QIDerivedTest::IDev, QIDerivedTest::IMid, QIDerivedTest::IBase, QIDerivedTest::DevImpl | OK |
| `QIDerivedTest::PartialImpl` | `tests/core/test_qi_route.cpp` | own | own | QIDerivedTest::backend::IDev | refuses unlisted ancestors (intended, test) |
| `QIDerivedTest::SkewImpl` | `tests/core/test_qi_route.cpp` | own | own | QIDerivedTest::IBase, QIDerivedTest::ISkew | OK |
| `QIRouteTest::Bar` | `tests/core/test_qi_route.cpp` | own | own (route parent Foo) | QIRouteTest::IA, QIRouteTest::Bar, QIRouteTest::IB, QIRouteTest::Foo | OK |
| `QIRouteTest::BarListed` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA, QIRouteTest::IB, QIRouteTest::BarListed, QIRouteTest::Foo | OK |
| `QIRouteTest::Foo` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA, QIRouteTest::IB, QIRouteTest::Foo | OK |
| `QIRouteTest::ForgotTable` | `tests/core/test_qi_route.cpp` | NOT DECLARED (rejected by MakeNewRCObj) | inherited: QIRouteTest::Foo | QIRouteTest::IA, QIRouteTest::IB, QIRouteTest::Foo | OK |
| `QIRouteTest::HandWritten` | `tests/core/test_qi_route.cpp` | - | HAND-WRITTEN QueryInterface (test stub) | (IObject only) | test stub: hand-written QueryInterface |
| `QIRouteTest::HandWrittenForgot` | `tests/core/test_qi_route.cpp` | - | INHERITS A HAND-WRITTEN QueryInterface (test) | (IObject only) | inherits a hand-written QueryInterface (test; rejected by MakeNewRCObj) |
| `QIRouteTest::InheritsTable` | `tests/core/test_qi_route.cpp` | inherit (declared) | inherited: QIRouteTest::Foo | QIRouteTest::IA, QIRouteTest::IB, QIRouteTest::Foo | OK |
| `QIRouteTest::Inner` | `tests/core/test_qi_route.cpp` | own | own | ND: QIRouteTest::IE, QIRouteTest::Inner | OK |
| `QIRouteTest::InnerEx` | `tests/core/test_qi_route.cpp` | own | own (route parent Inner) | ND: QIRouteTest::IE, QIRouteTest::InnerEx, QIRouteTest::Inner | OK |
| `QIRouteTest::InnerInherits` | `tests/core/test_qi_route.cpp` | inherit (declared) | inherited: QIRouteTest::InnerEx | ND: QIRouteTest::IE, QIRouteTest::InnerEx, QIRouteTest::Inner | OK |
| `QIRouteTest::InnerNoTable` | `tests/core/test_qi_route.cpp` | - | NO TABLE (abstract unless a subclass writes one) | (IObject only) | no table: abstract (intended, test) |
| `QIRouteTest::Leaf` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA, QIRouteTest::Leaf | OK |
| `QIRouteTest::Mid` | `tests/core/test_qi_route.cpp` | - | NO TABLE (abstract unless a subclass writes one) | (IObject only) | no table: abstract (intended, test) |
| `QIRouteTest::Multi` | `tests/core/test_qi_route.cpp` | own | own (route parent P1; route parent P2) | QIRouteTest::IA, QIRouteTest::Multi, QIRouteTest::P1, QIRouteTest::IB, QIRouteTest::P2 | OK |
| `QIRouteTest::OutOfLine` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA | OK |
| `QIRouteTest::Owner` | `tests/core/test_qi_route.cpp` | own | own (route member m_pInner (InnerEx)) | QIRouteTest::IA, QIRouteTest::IE, QIRouteTest::InnerEx, QIRouteTest::Inner | OK |
| `QIRouteTest::PrivateForgot` | `tests/core/test_qi_route.cpp` | NOT DECLARED (rejected by MakeNewRCObj) | inherited: QIRouteTest::PrivateTable | QIRouteTest::IA, QIRouteTest::PrivateTable | OK |
| `QIRouteTest::PrivateInherits` | `tests/core/test_qi_route.cpp` | inherit (declared) | inherited: QIRouteTest::PrivateTable | QIRouteTest::IA, QIRouteTest::PrivateTable | OK |
| `QIRouteTest::PrivateTable` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA, QIRouteTest::PrivateTable | OK |
| `QIRouteTest::ProtectedOutOfLine` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA | OK |
| `QIRouteTest::Shared` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IE / ND: QIRouteTest::IE | OK |
| `QIRouteTest::SharedOwner` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA | OK |
| `QIRouteTest::TemplateForgot` | `tests/core/test_qi_route.cpp` | NOT DECLARED (rejected by MakeNewRCObj) | TEMPLATE (audited through the classes derived from it) | (IObject only) | template (audited via derived classes) |
| `QIRouteTest::TemplateInherits` | `tests/core/test_qi_route.cpp` | inherit (declared, template) | TEMPLATE (audited through the classes derived from it) | (IObject only) | template (audited via derived classes) |
| `QIRouteTest::TemplateTable` | `tests/core/test_qi_route.cpp` | own (template) | TEMPLATE (audited through the classes derived from it) | (IObject only) | template (audited via derived classes) |
| `QIRouteTest::Two` | `tests/core/test_qi_route.cpp` | own | own | QIRouteTest::IA, QIRouteTest::IB | OK |
| `QIRouteTest::Weak` | `tests/core/test_qi_route.cpp` | own | own | IWeakReferenceSource, QIRouteTest::IA | OK |
| `QIRouteTest::WeakBar` | `tests/core/test_qi_route.cpp` | own | own (route parent WeakFoo) | IWeakReferenceSource, QIRouteTest::WeakBar | OK |
| `QIRouteTest::WeakFoo` | `tests/core/test_qi_route.cpp` | own | own | IWeakReferenceSource | OK |
| `QIRouteTest::WeakNoTable` | `tests/core/test_qi_route.cpp` | - | NO TABLE (abstract unless a subclass writes one) | (IObject only) | no table: abstract (intended, test) |
| `QIUser::App::XInner` | `tests/core/test_qi_route.cpp` | own | own | ND: QIUser::IY | OK |
| `QIUser::App::XLeaf` | `tests/core/test_qi_route.cpp` | own | own (route parent XRoot) | QIUser::IX, QIUser::IY | OK |
| `QIUser::App::XOwner` | `tests/core/test_qi_route.cpp` | own | own (route member m_pInner (XInner)) | QIUser::IX, QIUser::IY | OK |
| `QIUser::App::XRoot` | `tests/core/test_qi_route.cpp` | own | own (route parent XMix) | QIUser::IX, QIUser::IY | OK |
| `DefaultResource` | `tests/memory-queries.cpp` | own | own | IRHIObject | OK |

### donut (production code, 91 classes)

| Class | File | Declared | Table | Answers (besides IObject) | Result |
| --- | --- | --- | --- | --- | --- |
| `app::ApplicationBase` | `include/donut/app/ApplicationBase.h` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `app::DeviceManager` | `include/donut/app/DeviceManager.h` | own | own | (IObject only) | OK |
| `app::IRenderPass` | `include/donut/app/DeviceManager.h` | own | own | (IObject only) | OK |
| `DeviceManager_DX11` | `include/donut/app/DeviceManager_DX11.h` | inherit (declared) | inherited: app::DeviceManager | (IObject only) | OK |
| `DeviceManager_DX12` | `include/donut/app/DeviceManager_DX12.h` | inherit (declared) | inherited: app::DeviceManager | (IObject only) | OK |
| `DeviceManager_VK` | `include/donut/app/DeviceManager_VK.h` | inherit (declared) | inherited: app::DeviceManager | (IObject only) | OK |
| `app::ImGuiRenderPass` | `include/donut/app/ImGuiRenderPass.h` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `app::MediaFileSystem` | `include/donut/app/MediaFileSystem.h` | inherit (declared) | inherited: vfs::IFileSystem | (IObject only) | OK |
| `chunk::MeshSet` | `include/donut/core/chunk/chunk.h` | inherit (declared) | inherited: chunk::MeshSetBase | (IObject only) | OK |
| `chunk::MeshSetBase` | `include/donut/core/chunk/chunk.h` | own | own | (IObject only) | OK |
| `chunk::MeshletSet` | `include/donut/core/chunk/chunk.h` | inherit (declared) | inherited: chunk::MeshSetBase | (IObject only) | OK |
| `chunk::ChunkFile` | `include/donut/core/chunk/chunkFile.h` | own | own | (IObject only) | OK |
| `vfs::TarFile` | `include/donut/core/vfs/TarFile.h` | inherit (declared) | inherited: vfs::IFileSystem | (IObject only) | OK |
| `vfs::IFileSystem` | `include/donut/core/vfs/VFS.h` | own | own | (IObject only) | OK |
| `vfs::NativeFileSystem` | `include/donut/core/vfs/VFS.h` | own | own | vfs::NativeFileSystem | OK |
| `vfs::RelativeFileSystem` | `include/donut/core/vfs/VFS.h` | inherit (declared) | inherited: vfs::IFileSystem | (IObject only) | OK |
| `vfs::RootFileSystem` | `include/donut/core/vfs/VFS.h` | inherit (declared) | inherited: vfs::IFileSystem | (IObject only) | OK |
| `vfs::WinResFileSystem` | `include/donut/core/vfs/WinResFS.h` | inherit (declared) | inherited: vfs::IFileSystem | (IObject only) | OK |
| `engine::audio::AudioCache` | `include/donut/engine/AudioCache.h` | own | own | (IObject only) | OK |
| `engine::audio::AudioData` | `include/donut/engine/AudioCache.h` | own | own | (IObject only) | OK |
| `engine::audio::Effect` | `include/donut/engine/AudioEngine.h` | own | own | IWeakReferenceSource | OK |
| `engine::CommonRenderPasses` | `include/donut/engine/CommonRenderPasses.h` | own | own | (IObject only) | OK |
| `engine::console::Interpreter` | `include/donut/engine/ConsoleInterpreter.h` | own | own | (IObject only) | OK |
| `engine::DescriptorHandle` | `include/donut/engine/DescriptorTableManager.h` | own | own | (IObject only) | OK |
| `engine::DescriptorTableManager` | `include/donut/engine/DescriptorTableManager.h` | own | own | IWeakReferenceSource | OK |
| `engine::FramebufferFactory` | `include/donut/engine/FramebufferFactory.h` | own | own | (IObject only) | OK |
| `engine::IesProfile` | `include/donut/engine/IesProfile.h` | own | own | (IObject only) | OK |
| `engine::animation::Sampler` | `include/donut/engine/KeyframeAnimation.h` | own | own | (IObject only) | OK |
| `engine::MaterialBindingCache` | `include/donut/engine/MaterialBindingCache.h` | own | own | (IObject only) | OK |
| `engine::Scene` | `include/donut/engine/Scene.h` | own | own | (IObject only) | OK |
| `engine::DirectionalLight` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::Light` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::MeshInstance` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::OrthographicCamera` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::PerspectiveCamera` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::PointLight` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::SceneCamera` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::SceneGraph` | `include/donut/engine/SceneGraph.h` | own | own | IWeakReferenceSource | OK |
| `engine::SceneGraphAnimation` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::SceneGraphAnimationChannel` | `include/donut/engine/SceneGraph.h` | own | own | (IObject only) | OK |
| `engine::SceneGraphLeaf` | `include/donut/engine/SceneGraph.h` | own | own | IWeakReferenceSource | OK |
| `engine::SceneGraphNode` | `include/donut/engine/SceneGraph.h` | own | own | IWeakReferenceSource | OK |
| `engine::SceneTypeFactory` | `include/donut/engine/SceneGraph.h` | own | own | (IObject only) | OK |
| `engine::SkinnedMeshInstance` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::SkinnedMeshReference` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::SpotLight` | `include/donut/engine/SceneGraph.h` | inherit (declared) | inherited: engine::SceneGraphLeaf | IWeakReferenceSource | OK |
| `engine::BufferGroup` | `include/donut/engine/SceneTypes.h` | own | own | (IObject only) | OK |
| `engine::GltfInlineData` | `include/donut/engine/SceneTypes.h` | own | own | (IObject only) | OK |
| `engine::LightProbe` | `include/donut/engine/SceneTypes.h` | own | own | (IObject only) | OK |
| `engine::LoadedTexture` | `include/donut/engine/SceneTypes.h` | own | own | (IObject only) | OK |
| `engine::Material` | `include/donut/engine/SceneTypes.h` | own | own | IWeakReferenceSource | OK |
| `engine::MeshGeometry` | `include/donut/engine/SceneTypes.h` | own | own | (IObject only) | OK |
| `engine::MeshInfo` | `include/donut/engine/SceneTypes.h` | own | own | (IObject only) | OK |
| `engine::ShaderFactory` | `include/donut/engine/ShaderFactory.h` | own | own | (IObject only) | OK |
| `engine::IShadowMap` | `include/donut/engine/ShadowMap.h` | own | own | (IObject only) | OK |
| `engine::TextureCache` | `include/donut/engine/TextureCache.h` | own | own | (IObject only) | OK |
| `engine::TextureData` | `include/donut/engine/TextureCache.h` | inherit (declared) | inherited: engine::LoadedTexture | (IObject only) | OK |
| `engine::ThreadPoolTask` | `include/donut/engine/ThreadPool.h` | own | own | (IObject only) | OK |
| `engine::CompositeView` | `include/donut/engine/View.h` | inherit (declared) | inherited: engine::ICompositeView | (IObject only) | OK |
| `engine::CubemapView` | `include/donut/engine/View.h` | inherit (declared) | inherited: engine::ICompositeView | (IObject only) | OK |
| `engine::ICompositeView` | `include/donut/engine/View.h` | own | own | (IObject only) | OK |
| `engine::IView` | `include/donut/engine/View.h` | inherit (declared) | inherited: engine::ICompositeView | (IObject only) | OK |
| `engine::PlanarView` | `include/donut/engine/View.h` | inherit (declared) | inherited: engine::ICompositeView | (IObject only) | OK |
| `engine::StereoView` | `include/donut/engine/View.h` | inherit (declared, template) | TEMPLATE (audited through the classes derived from it) | (IObject only) | template (audited via derived classes) |
| `render::BloomPass` | `include/donut/render/BloomPass.h` | own | own | (IObject only) | OK |
| `render::CascadedShadowMap` | `include/donut/render/CascadedShadowMap.h` | inherit (declared) | inherited: engine::IShadowMap | (IObject only) | OK |
| `render::DLSS` | `include/donut/render/DLSS.h` | own | own | (IObject only) | OK |
| `render::DeferredLightingPass` | `include/donut/render/DeferredLightingPass.h` | own | own | (IObject only) | OK |
| `render::DepthPass` | `include/donut/render/DepthPass.h` | inherit (declared) | inherited: render::IGeometryPass | (IObject only) | OK |
| `render::IDrawStrategy` | `include/donut/render/DrawStrategy.h` | own | own | (IObject only) | OK |
| `render::InstancedOpaqueDrawStrategy` | `include/donut/render/DrawStrategy.h` | inherit (declared) | inherited: render::IDrawStrategy | (IObject only) | OK |
| `render::PassthroughDrawStrategy` | `include/donut/render/DrawStrategy.h` | inherit (declared) | inherited: render::IDrawStrategy | (IObject only) | OK |
| `render::TransparentDrawStrategy` | `include/donut/render/DrawStrategy.h` | inherit (declared) | inherited: render::IDrawStrategy | (IObject only) | OK |
| `render::ForwardShadingPass` | `include/donut/render/ForwardShadingPass.h` | inherit (declared) | inherited: render::IGeometryPass | (IObject only) | OK |
| `render::GBufferFillPass` | `include/donut/render/GBufferFillPass.h` | inherit (declared) | inherited: render::IGeometryPass | (IObject only) | OK |
| `render::MaterialIDPass` | `include/donut/render/GBufferFillPass.h` | inherit (declared) | inherited: render::IGeometryPass | (IObject only) | OK |
| `render::IGeometryPass` | `include/donut/render/GeometryPasses.h` | own | own | (IObject only) | OK |
| `render::PlanarShadowMap` | `include/donut/render/PlanarShadowMap.h` | inherit (declared) | inherited: engine::IShadowMap | (IObject only) | OK |
| `render::SkyPass` | `include/donut/render/SkyPass.h` | own | own | (IObject only) | OK |
| `render::SsaoPass` | `include/donut/render/SsaoPass.h` | own | own | (IObject only) | OK |
| `render::TemporalAntiAliasingPass` | `include/donut/render/TemporalAntiAliasingPass.h` | own | own | (IObject only) | OK |
| `render::ToneMappingPass` | `include/donut/render/ToneMappingPasses.h` | own | own | (IObject only) | OK |
| `engine::audio::Xaudio2Effect` | `src/engine/AudioEngine.cpp` | inherit (declared) | inherited: engine::audio::Effect | IWeakReferenceSource | OK |
| `engine::audio::Xaudio2Effect3D` | `src/engine/AudioEngine.cpp` | inherit (declared) | inherited: engine::audio::Effect | IWeakReferenceSource | OK |
| `engine::GltfImporter` | `src/engine/SceneImporterImpl.h` | own | own | engine::ISceneImporter, engine::GltfImporter | OK |
| `StbImageBlob` | `src/engine/TextureCache.cpp` | own | own | IDataBlob | OK |
| `engine::ThreadPoolFunctionTask` | `src/engine/ThreadPool.cpp` | inherit (declared) | inherited: engine::ThreadPoolTask | (IObject only) | OK |
| `DLSS_DX11` | `src/render/DLSS-DX11.cpp` | inherit (declared) | inherited: render::DLSS | (IObject only) | OK |
| `DLSS_DX12` | `src/render/DLSS-DX12.cpp` | inherit (declared) | inherited: render::DLSS | (IObject only) | OK |
| `DLSS_VK` | `src/render/DLSS-VK.cpp` | inherit (declared) | inherited: render::DLSS | (IObject only) | OK |
| `MipMapGenPass::NullTextures` | `src/render/MipMapGenPass.cpp` | own | own | (IObject only) | OK |

### etherealsamples (production code, 52 classes)

| Class | File | Declared | Table | Answers (besides IObject) | Result |
| --- | --- | --- | --- | --- | --- |
| `AsteroidsRenderer` | `benchmark/Asteroids/nvrhi/renderer.h` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `DDGISample` | `src/DDGISample/DDGISample.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `SampleScene` | `src/DDGISample/SampleScene.h` | inherit (declared) | inherited: engine::Scene | (IObject only) | OK |
| `UIPass` | `src/DDGISample/UIPass.h` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `gp::cuda::Buffer` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own (route parent DeviceChild) | gp::IBuffer, gp::IResource, gp::IDeviceChild | OK |
| `gp::cuda::Device` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own | gp::IDevice | OK |
| `gp::cuda::DeviceChild` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own (template) | TEMPLATE (audited through the classes derived from it) | (IObject only) | template (audited via derived classes) |
| `gp::cuda::DeviceQueue` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own (route parent DeviceChild) | gp::IDeviceQueue, gp::IDeviceChild | OK |
| `gp::cuda::GraphicsInteropSemaphore` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own (route parent DeviceChild) | gp::IGraphicsInteropSemaphore, gp::IDeviceChild | OK |
| `gp::cuda::Kernel` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own | gp::IKernel, gp::IDeviceChild / ND: gp::IKernel, gp::IDeviceChild | OK |
| `gp::cuda::Module` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own (route parent DeviceChild) | gp::IModule, gp::IDeviceChild | OK |
| `gp::cuda::Texture` | `src/GVDBSamples/gvdb/GPDeviceCUDAImpl.h` | own | own (route parent DeviceChild) | gp::ITexture, gp::IResource, gp::IDeviceChild | OK |
| `GPAndNVRHIInteropDevice` | `src/GVDBSamples/gvdb/GPDeviceNVRHI.cpp` | own | own | IGPAndNVRHIInteropDevice | OK |
| `gvdb::GVDB` | `src/GVDBSamples/gvdb/GVDB.h` | own | own | (IObject only) | OK |
| `DepthMapPass` | `src/GVDBSamples/samples/depth_map/DepthMapPass.cpp` | own | own (route parent IRenderPass) | DepthMapPass | OK |
| `VoxelizeGUIPass` | `src/GVDBSamples/samples/mesh_voxelize/MeshVoxelizePass.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `VoxelizePass` | `src/GVDBSamples/samples/mesh_voxelize/MeshVoxelizePass.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `SampleUtils::Camera` | `src/GVDBSamples/samples/sample_utils/Camera.h` | own | own | (IObject only) | OK |
| `SampleUtils::Light` | `src/GVDBSamples/samples/sample_utils/Camera.h` | inherit (declared) | inherited: SampleUtils::Camera | (IObject only) | OK |
| `SampleUtils::GPDeviceMessageCallback` | `src/GVDBSamples/samples/sample_utils/SampleTypes.h` | own | own | gp::IMessageCallback | OK |
| `SampleUtils::Model` | `src/GVDBSamples/samples/sample_utils/Scene.h` | own | own | (IObject only) | OK |
| `SampleUtils::Scene` | `src/GVDBSamples/samples/sample_utils/Scene.h` | own | own | (IObject only) | OK |
| `SampleUtils::NamedKernels` | `src/GVDBSamples/samples/sample_utils/Voxelizer.cpp` | own | own | (IObject only) | OK |
| `SampleUtils::Voxelizer` | `src/GVDBSamples/samples/sample_utils/Voxelizer.cpp` | own | own | SampleUtils::IVoxelizer | OK |
| `SampleDeferredLightingPass` | `src/VXGISample/SampleDeferredLightingPass.h` | inherit (declared) | inherited: render::DeferredLightingPass | (IObject only) | OK |
| `SampleGBufferFillPass` | `src/VXGISample/SampleGBufferFillPass.h` | inherit (declared) | inherited: render::IGeometryPass | (IObject only) | OK |
| `UIPass` | `src/VXGISample/VXGISample.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `VXGISample` | `src/VXGISample/VXGISample.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `vxgi::ViewTracer` | `src/VXGISample/ViewTracer.h` | own | own | (IObject only) | OK |
| `vxgi::VoxelRenderer` | `src/VXGISample/VoxelRenderer.h` | own | own | (IObject only) | OK |
| `vxgi::VoxelShadingPass` | `src/VXGISample/VoxelShadingPass.h` | inherit (declared) | inherited: render::IGeometryPass | (IObject only) | OK |
| `vxgi::VoxelizationInstancedDrawStrategy` | `src/VXGISample/VoxelShadingPass.h` | inherit (declared) | inherited: render::IDrawStrategy | (IObject only) | OK |
| `vxgi::VoxelizationView` | `src/VXGISample/VoxelShadingPass.h` | inherit (declared) | inherited: engine::ICompositeView | (IObject only) | OK |
| `AftermathSample` | `src/aftermath/aftermath.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `UIRenderer` | `src/aftermath/aftermath.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `AsyncCompute` | `src/async_compute/async_compute.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `BasicTriangle` | `src/basic_triangle/basic_triangle.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `BindlessRendering` | `src/bindless_rendering/bindless_rendering.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `DeferredShading` | `src/deferred_shading/deferred_shading.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `MeshletExample` | `src/meshlets/meshlets.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `BindlessRayTracing` | `src/rt_bindless/rt_bindless.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `RayTracedParticles` | `src/rt_particles/rt_particles.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `UserInterface` | `src/rt_particles/rt_particles.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `VariableRateShading` | `src/rt_reflections/rt_reflections.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `RayTracedShadows` | `src/rt_shadows/rt_shadows.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `RayTracedTriangle` | `src/rt_triangle/rt_triangle.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `ShaderSpecializations` | `src/shader_specializations/shader_specializations.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `ThreadedRendering` | `src/threaded_rendering/threaded_rendering.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `VariableRateShading` | `src/variable_shading/variable_shading.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `VertexBuffer` | `src/vertex_buffer/vertex_buffer.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `UIRenderer` | `src/work_graphs/work_graphs_d3d12.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
| `WorkGraphs` | `src/work_graphs/work_graphs_d3d12.cpp` | inherit (declared) | inherited: app::IRenderPass | (IObject only) | OK |
