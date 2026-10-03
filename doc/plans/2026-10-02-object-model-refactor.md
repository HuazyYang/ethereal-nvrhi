# Object model refactor: `nvrhi::core`, `IRHIObject`, `AutoPtr`

- Status: Done
- Approved: 2026-10-02
- Completed: 2026-10-02
- Decisions: [ADR 0001](../adr/0001-com-object-model-for-nvrhi.md),
  [0002](../adr/0002-irhiobject-root-interface-and-iids.md),
  [0003](../adr/0003-queryinterface-interface-chains.md),
  [0004](../adr/0004-autoptr-replaces-refcountptr.md),
  [0005](../adr/0005-backends-use-monoptr-and-autoptr.md)
- Follow-up: [ADR 0006](../adr/0006-no-rtti-queryinterface-casts.md) (no RTTI; section 6)

This is the archived implementation plan for the object model refactor. Sections 1-3 restate the plan
as it was approved, edited only for the repository (no local paths). Section 4 records what was
actually done, step by step, with commits and deviations. Sections 5 and 6 record verification and the
issues found. Where the plan and the code differ, the code is authoritative and the difference is listed
under "Deviations".

## 1. Context

NVRHI had its own reference counting (`IResource`, `RefCountPtr<T>`, `RefCounter<T>`) and used std
smart pointers internally. donut had a separate COM-like object model (`IObject`, UUIDs,
`AutoPtr`/`WeakPtr`/`MonoPtr`) in `include/donut/core/object`. The goal was a single object model, owned
by NVRHI, that donut builds on:

- move donut's `core/object` headers into NVRHI as `nvrhi::core`;
- re-base every NVRHI interface on `nvrhi::IObject`, with a UUID;
- drop `RefCountPtr`/`RefCounter` in favor of `AutoPtr`.

Repositories and branches:

| Repository | Path in the aggregate | Branch |
| --- | --- | --- |
| NVRHI | `donut/nvrhi` | `lithereal-dev` (12 commits behind `origin/main` at the start, none of its own) |
| donut (fork) | `donut` | `ethereal-dev` |
| etherealsamples | `etherealsamples` | `main` |
| aggregate root | `.` | `main` (submodule pointer bumps only) |

Builds use the root `build.ps1` from PowerShell; it sets up the Visual Studio environment (VsDevCmd)
itself. Do not build from a POSIX shell: without `INCLUDE`/`LIB`, Ninja may report "up to date" without
compiling anything.

## 2. Decisions taken at approval

- `AutoPtr<T>` keeps its name. There is no `ComPtr`.
- `RefCountPtr<T>` and `RefCounter<T>` are deleted at the end.
- A new base `IRHIObject : IObject` carries `getNativeObject()` and `queryMemoryRequirements()`.
- Native D3D/DXGI COM objects also use `nvrhi::AutoPtr`.
- donut is updated directly. There are no forwarding shims.

## 3. Plan as approved

### Step 1: Merge `origin/main` into `lithereal-dev` (NVRHI)

1. `git fetch origin`, then `git merge origin/main` on `lithereal-dev`: a fast-forward from `c8d34b4` to
   `de5defc`.
2. The merge brings in `IResource::queryMemoryRequirements` and `struct MemoryRequirements`,
   `cmake/NvrhiTargetArch.cmake`, the `NVRHI_BUILD_TESTS` option with `tests/memory-queries.cpp`, and
   Arm64 NVAPI/Aftermath support.
3. Build donut and the samples once against the merged NVRHI for a green baseline. Push only when asked.

### Step 2: Move `core/object` into NVRHI as `nvrhi::core`

- **Placement.** `donut/include/donut/core/object/{AutoPtr,DataBlob,Foundation,Memory,Threading,Types}.h`
  move to `include/nvrhi/core/`. All six are header only.
- **Renames**, applied mechanically:
  - namespaces `donut`, `donut::details`, `donut::literals` to `nvrhi`, `nvrhi::details`,
    `nvrhi::literals`;
  - every `DONUT_*` macro to `NVRHI_*`; include guards to `NVRHI_CORE_*_H`;
  - `donut_likely`/`donut_unlikely` to `NVRHI_LIKELY`/`NVRHI_UNLIKELY`; `_donut_guid` to `_nvrhi_guid`;
    `DonutNewOverload` to `NvrhiNewOverload`;
  - includes `<donut/core/object/X.h>` to `<nvrhi/core/X.h>`.
- **Hygiene fixes while moving:** missing std includes; `autoptr.h` includes `memory.h`; remove the
  duplicate likely/unlikely block in `foundation.h`; fix `QIT_` in `RouteMemberQueryInterface`; fix the
  inverted `WeakReferenceImpl::IsExpired`; fix `std::hash<WeakPtr>`; fix the array `MonoPtr` default
  constructor's `Dx2` and its move constructor tag.
- **Global helpers:** `UUIDTraits` to `nvrhi::details::UUIDTraits`, `__uuid_of` to `nvrhi::uuid_of`,
  `FIID_PPV_ARGS` to `NVRHI_IID_PPV_ARGS`, `FSUCCEEDED`/`FFAILED` to `NVRHI_SUCCEEDED`/`NVRHI_FAILED`.
  Keep the Windows SDK `IID_PPV_ARGS_Helper` overload for `AutoPtrRef`.
- **CMake** (in `CMakeLists.txt`): an `include_core` list; `add_library(nvrhi_core STATIC
  src/core/core.cpp ...)` with alias `nvrhi::core`, where `core.cpp` holds an out-of-line
  `GetDefaultMemAllocator()` so there is a real object file; public include directory, `cxx_std_17`,
  Threads; link `nvrhi_core` PUBLIC from `nvrhi`, `nvrhi_d3d11`, `nvrhi_d3d12`, `nvrhi_vk`; add it to
  the install export; IDE groups.
- **Tests:** move `test_auto_ptr.cpp` and `test_qi_route.cpp` to `tests/core/`, gated by
  `NVRHI_BUILD_TESTS`, GTest via `find_package(GTest CONFIG)`, VLD optional via `VLD_INSTALL_DIR`.
- **donut, same change:** rewrite includes and names (about 45 files); delete
  `include/donut/core/object/`; drop the headers from `donut-core.cmake` and link `nvrhi::core`; keep
  `donut_core` buildable with `DONUT_WITH_NVRHI=OFF`; remove the moved tests from `tests/test-core.cmake`.

### Step 3: Re-base the NVRHI interfaces on `IObject`

- **3a. Root interface.** In `include/nvrhi/common/resource.h`, add `IRHIObject : IObject` with
  `getNativeObject` and `queryMemoryRequirements`, and `typedef AutoPtr<IRHIObject> RHIObjectHandle`.
  Drop `GetRefCount()`. Delete `IResource`, `RefCountPtr`, `ResourceHandle`, `RefCounter` last, so each
  intermediate stage builds.
- **3b. Public interfaces.** The 29 interfaces (25 in `nvrhi.h`; `d3d12::IRootSignature`,
  `d3d12::ICommandList`, `d3d12::IDevice`; `vulkan::IDevice`) become `struct IX : IRHIObject` with
  `NVRHI_IID` and `NVRHI_DECLARE_UUID_TRAITS`; fresh lowercase v4 UUIDs recorded in a comment table;
  `*Handle` typedefs become `AutoPtr<IX>`. Out of scope: `IMessageCallback`, `d3d12::IDescriptorHeap`,
  `TextureStateExtension`, `BufferStateExtension`, `vulkan::MemoryResource`.
  `BindingSetItem::resourceHandle` becomes `IRHIObject*`; remove `std::hash<RefCountPtr<T>>`.
- **3c. Implementations.** About 74 `RefCounter<...>` classes become `ObjectImpl<...>`. Interface
  chains (`d3d12::Device`, `IDescriptorTable : IBindingSet`) and multi-interface classes (d3d12
  `ShaderLibraryEntry`, `BindlessLayout`, validation `DeviceWrapper`, `CommandListWrapper`) get explicit
  `NVRHI_BEGIN_INTERFACE_TABLE`s, with the pattern settled on the d3d12 device first. `new X` plus
  `Handle::Create` (84 sites) become `MAKE_RC_OBJ` plus `TakeOver`. About 110 native COM uses and 40
  concrete-class handles move to `AutoPtr`. Qualify Windows `GUID` as `::GUID` where `nvrhi::GUID`
  shadows it. Update `tools/nvrhi.natvis`, `doc/ProgrammingGuide.md` and `doc/Tutorial.md`.
- **3d. std smart pointers in `src/`** (best effort). `std::unique_ptr`/`make_unique` to
  `MonoPtr`/`MakeMono`; `std::shared_ptr`/`make_shared` to `AutoPtr`/`MAKE_RC_OBJ_PTR`, with each
  pointee an `ObjectImpl<IObject>` with an `NVRHI_CCLSID` (d3d12/vulkan `BufferChunk`, d3d12
  `InternalCommandList`, `CommandListInstance`, vulkan `TrackedCommandBuffer`). Keep a std pointer, with
  a comment, where intrusive ownership of a third-party type is impossible.
- **3e. donut consumers.** `DeviceManager_DX11/DX12` and `StreamlineIntegration.h` move from
  `nvrhi::RefCountPtr<IDXGI*/ID3D12*>` to `nvrhi::AutoPtr`. etherealsamples is expected to need only a
  rebuild.

### Execution order (as approved)

1. NVRHI: fast-forward merge (step 1).
2. NVRHI: `nvrhi::core` and the tests; donut: switch to `nvrhi::core`. Build both.
3. NVRHI: `IRHIObject` and UUIDs; convert d3d12 first, then d3d11, vulkan, validation; delete
   `RefCounter`/`RefCountPtr` after the last backend compiles.
4. NVRHI: std smart pointer pass (3d).
5. donut: DeviceManager fixes (3e). Aggregate: bump the submodule pointers.

Commit each step in the submodule first, then bump the pointers in the parent. Do not push unless asked.

### Verification (as approved)

- **Build:** `build.ps1` for the full aggregate, static and `NVRHI_BUILD_SHARED=ON`; check that targets
  actually recompiled.
- **Unit tests:** `NVRHI_BUILD_TESTS=ON`, `ctest`: `test_auto_ptr`, `test_qi_route`,
  `nvrhi_memory_queries` (d3d11, d3d12, vulkan), donut's remaining core tests.
- **New QI test:** per backend, `QueryInterface` for `IObject`, `IRHIObject`, `IDevice` and the backend
  device IID returns the same object; an unrelated IID is refused; reference counts balance.
- **Runtime:** several samples on DX11, DX12 and Vulkan with the validation layer and debug layers on;
  look for leaks and lifetime errors at shutdown.
- **Final grep:** no `RefCountPtr`, `RefCounter<`, `IResource`, `donut/core/object` or `DONUT_IID` in
  NVRHI or donut; report remaining std smart pointers.

## 4. Execution record

### Commits

In commit order. "Points to" gives the submodule commit recorded by a pointer bump.

| # | Repository | Commit | Subject | Points to |
| --- | --- | --- | --- | --- |
| 1 | nvrhi | `de5defc` | HOST and TARGET (upstream tip; fast-forward from `c8d34b4`) | |
| 2 | nvrhi | `9eb1458` | core: add nvrhi::core object model (moved from donut core/object) | |
| 3 | donut | `8ec223c` | core: use nvrhi::core object model; remove donut/core/object | nvrhi `9eb1458` |
| 4 | etherealsamples | `925c412` | Use nvrhi::core object model (moved from donut core/object) | |
| 5 | root | `347d6b0` | bump donut (nvrhi::core) | donut `8ec223c`, etherealsamples `925c412` |
| 6 | nvrhi | `ce4a071` | core: interface chains for QueryInterface | |
| 7 | nvrhi | `a500ab7` | Re-base the NVRHI interfaces on nvrhi::IObject | |
| 8 | nvrhi | `54e011f` | tests: QueryInterface on real backend objects | |
| 9 | nvrhi | `ed06c3c` | doc, natvis: AutoPtr / IObject / IRHIObject | |
| 10 | donut | `3544c7b` | app: nvrhi::AutoPtr for native D3D/DXGI interfaces; bump nvrhi | nvrhi `ed06c3c` |
| 11 | etherealsamples | `1ad7a40` | asteroids_nvrhi: nvrhi::AutoPtr for native D3D/DXGI interfaces | |
| 12 | root | `e661df1` | bump donut, etherealsamples (nvrhi interfaces on nvrhi::IObject) | donut `3544c7b`, etherealsamples `1ad7a40` |
| 13 | nvrhi | `8f810ce` | core: MonoPtr operator*, throwing MakeMono, Derived->Base DefaultDeleter | |
| 14 | nvrhi | `23ee0df` | Replace std smart pointers with MonoPtr / AutoPtr in backends | |
| 15 | donut | `236b594` | bump nvrhi (std smart pointers -> MonoPtr/AutoPtr) | nvrhi `23ee0df` |
| 16 | root | `7d26c54` | bump donut (nvrhi: std smart pointers -> MonoPtr/AutoPtr) | donut `236b594` |

Nothing was pushed. At the time of writing, `lithereal-dev` is 19 commits ahead of
`origin/lithereal-dev` (the 12 merged upstream commits and the 7 listed above), donut `ethereal-dev` is
3 ahead, etherealsamples `main` 2 ahead, and the root `main` 3 ahead.

### Step 1: Fast-forward merge

**Done.** `lithereal-dev` was fast-forwarded from `c8d34b4` to `de5defc` (12 upstream commits; the
reflog entry is "merge origin/main: Fast-forward").

**Deviations.** None in the merge itself. This record has no log of the separate baseline build before
step 2.

### Step 2: `nvrhi::core`

**Done** in nvrhi `9eb1458`, donut `8ec223c`, etherealsamples `925c412`, root `347d6b0`. Details and
rationale: [ADR 0001](../adr/0001-com-object-model-for-nvrhi.md).

**Deviations.**

- **CMake lives in `cmake/NvrhiCore.cmake`**, included from `CMakeLists.txt`, not inline. This lets
  donut add only the core library when `DONUT_WITH_NVRHI=OFF` (`donut-core.cmake` includes the file if
  `nvrhi::core` does not exist yet). `src/nvrhiConfig.cmake.in` gained `find_dependency(Threads)`.
- **`core.cpp` is a compile-check translation unit**, not an out-of-line allocator. It includes all six
  headers, holds `static_assert`s, and defines `nvrhi::details::GetCoreLibraryName()` as the library's
  only public symbol (avoids MSVC LNK4221). `GetDefaultMemAllocator()` stays inline in `memory.h`; see
  the comment there (one instance per module on Windows is fine for a stateless allocator).
- **`UUIDTraits` became the global `NvrhiUUIDTraits`**, not `nvrhi::details::UUIDTraits`: `NVRHI_IID`
  specializes it from arbitrary namespaces, which MSVC rejects for a template in a named namespace
  (C2888). The lookup function did move: `nvrhi::uuid_of`.
- **The `QIT_` typo** was fixed to `QIT` (the template parameter of `RouteMemberQueryInterface`), not
  `QIB_` as the plan text said.
- **More hygiene fixes than planned** were needed for a clean /W4 library build: the self-referencing
  `QITraits` member alias in the root and pass-through layers; the const `CompressedPair::GetFirst`
  returning a non-const reference; `ObjectWrapperStorage` instantiating `ObjectWrapper<IObject, ...>`;
  `fetch_add(-1)` on an unsigned counter (now `fetch_sub(1)`); `strncpy` in `StringDataBlobImpl`;
  unused parameters.
- **GTest is optional**: `find_package(GTest CONFIG QUIET)`; without it the two core tests are skipped
  with a status message. Pass `-DGTest_DIR=...`. VLD is enabled by the environment variable
  `VLD_INSTALL_DIR`.
- **etherealsamples needed source changes** for the rename (`925c412`: DDGISample, VXGISample,
  GVDBSamples, async_compute, aftermath, the Asteroids benchmark and others). The plan expected
  etherealsamples to need only a rebuild, which was true only for step 3.
- **donut touched about 90 files**, not 45 (headers under `include/donut/{app,core,engine,render}` as
  well as sources and tests).

### Interface chains (between steps 2 and 3)

**Done** in nvrhi `ce4a071`, as its own core commit before the interface rebase. Details:
[ADR 0003](../adr/0003-queryinterface-interface-chains.md).

**Deviation.** The plan said to check how `QIQueryRoot` handles base-of-base IIDs during the d3d12
device conversion and to write explicit interface tables. It did not handle them, so instead of tables,
`NVRHI_DECLARE_UUID_TRAITS_DERIVED(Interface, Parent)` and `NVRHI_IMPLEMENTS_INTERFACE_CHAIN` were added
to the core, with seven tests. The same commit made `NVRHI_IID` forward-declare the interface, which
`d3d12::IDevice` and `vulkan::IDevice` need.

### Step 3: Re-base the interfaces (3a, 3b, 3c, 3e)

**Done** in nvrhi `a500ab7` (headers and all backends), `54e011f` (QI test), `ed06c3c` (docs, natvis);
donut `3544c7b`; etherealsamples `1ad7a40`; root `e661df1`. Details:
[ADR 0002](../adr/0002-irhiobject-root-interface-and-iids.md),
[ADR 0004](../adr/0004-autoptr-replaces-refcountptr.md).

**Deviations.**

- **Headers and all four backends went in one commit (`a500ab7`)**, not d3d12 first with
  `RefCounter`/`RefCountPtr` kept until the last backend compiled. Changing the public headers breaks
  every backend at once, so a single commit is the smallest unit that builds. Every commit in the series
  builds.
- **No explicit interface tables were needed.** With interface chains (above), every backend class
  lists only its most derived interface in `ObjectImpl<...>`. The classes the plan expected to have
  several interfaces (`ShaderLibraryEntry`, `BindlessLayout`, `DeviceWrapper`, `CommandListWrapper`)
  implement one interface each.
- **IID naming.** The plan named the backend IIDs `IID_d3d12_IDevice` / `IID_vulkan_IDevice`. The code
  uses namespaced names generated by `NVRHI_IID`: `nvrhi::IID_IDevice`, `nvrhi::d3d12::IID_IDevice`,
  `nvrhi::vulkan::IID_IDevice`. The "no interface" code is `FE_NOINTERFACE` (the plan wrote
  `FE_NO_INTERFACE`).
- **Interfaces use `NVRHI_DECLARE_UUID_TRAITS_DERIVED`**, not plain `NVRHI_DECLARE_UUID_TRAITS`, so that
  the chain is answered.
- **No UUID comment table in the headers.** The table of interfaces and IIDs is in ADR 0002.
- **`::GUID` qualification was not needed.** No backend source used an unqualified Windows `GUID` inside
  `namespace nvrhi`.
- **Error paths:** `delete x` on partially constructed objects became `x->Release()`.
- **`d3d12::Device::getNativeObject(Nvrhi_D3D12_Device)`** returns
  `static_cast<nvrhi::d3d12::IDevice*>(this)`.
- **The single-argument `IUnknown::QueryInterface(&p)`** does not deduce through `AutoPtr`'s
  `operator&`; those calls became `QueryInterface(IID_PPV_ARGS(&p))` (five in
  `src/d3d12/d3d12-device.cpp`, one in donut `DeviceManager_DX12.cpp`).
- **Step 3e was done together with step 3** (donut `3544c7b`), because donut does not compile against an
  NVRHI without `RefCountPtr`. The Asteroids benchmark in etherealsamples also used
  `nvrhi::RefCountPtr` and was converted (`1ad7a40`).
- **The QI test is `tests/qi-device.cpp`** (target `nvrhi_qi_device`, tests `qi_device_d3d11`,
  `qi_device_d3d12`, `qi_device_vulkan`), next to `memory-queries.cpp`, not under `tests/core/`. It
  creates headless devices (WARP for D3D11) and also checks a texture, buffer, command list and
  descriptor table, directly and through the validation layer.
- **Docs:** `doc/ProgrammingGuide.md` and `doc/memory-queries.md` were updated. `doc/Tutorial.md` named
  none of the removed types and was left unchanged.
- **Not done (optional in the plan):** replacing `checked_cast` on `BindingSetItem::resourceHandle` in
  the state-tracking and validation code with `QueryInterface`.

### Step 4: std smart pointers (3d)

**Done** in nvrhi `8f810ce` (core) and `23ee0df` (backends); donut `236b594`; root `7d26c54`. Details:
[ADR 0005](../adr/0005-backends-use-monoptr-and-autoptr.md).

**Deviations.**

- **Core changes were needed first** (`8f810ce`): `MonoPtr::operator*`, `MakeMono` no longer
  `noexcept`, and the `DefaultDeleter` Derived-to-Base conversion fix, with five new tests.
- **The pooled objects have a minimal interface table** answering their own class ID
  (`NVRHI_IMPLEMENTS_INTERFACE(X)` plus `..._ROUTE_PARENT()`), are `final`, and are declared before
  their `NVRHI_CCLSID` (the macro does not forward-declare). Vulkan `BufferChunk` is a struct and uses
  `NVRHI_SCLSID`.
- **The D3D12 upload and scratch managers** were already by-value members; only the Vulkan ones were
  `std::unique_ptr`.
- **RTXMU code was converted but not compiled**: it is inside `#ifdef NVRHI_WITH_RTXMU`, the option is
  OFF, and RTXMU is fetched with `FetchContent` (no local copy).
- **No std pointer had to stay.** No `std::unique_ptr`, `std::shared_ptr`, `make_*` or `weak_ptr`
  remains in `src/`.

### Step 5: donut DeviceManager and pointer bumps

**Done** as part of step 3 (donut `3544c7b`) and by the pointer bumps listed in the commit table.

## 5. Verification

The results below come from the validation session that accompanied the work. They were not rerun when
this record was written.

**Build configuration.** Validation runs used **Release** builds, because the samples do not link in
Debug (a ShaderTool issue in the aggregate, unrelated to this work). The aggregate was built with
`build.ps1`; the NVRHI tests need `-DNVRHI_BUILD_TESTS=ON` and, for the GTest core tests,
`-DGTest_DIR=...`.

### Unit tests (static build)

`ctest` on the aggregate build (run of 2026-10-02 08:32 local time; it already contains the
`Common_MonoPtr` tests from `8f810ce`): **10 of 11 pass**.

| Test | Result |
| --- | --- |
| `nvrhi_test_auto_ptr` | Pass (16 tests in 4 suites) |
| `nvrhi_test_qi_route` | Pass (16 tests: 9 `QueryInterfaceRoute`, 7 `QueryInterfaceChain`) |
| `qi_device_d3d11` | Pass |
| `qi_device_d3d12` | Pass |
| `qi_device_vulkan` | Pass |
| `memory_queries_d3d11` | Pass |
| `memory_queries_vulkan` | Pass |
| `memory_queries_d3d12` | **Fail**, pre-existing: `FAIL: AS query (including validation wrapper)`, `CreateCommittedResource ... HRESULT = 0x80070057` (see section 6) |
| `ShaderToolBlobTests` | Pass (aggregate test, not part of this work) |
| `benchmark_selftest` | Pass (aggregate test) |
| `benchmark_analysis` | Pass (aggregate test) |

The `qi_device_*` tests cover the new QI requirements of the plan: `IObject`, `IRHIObject`, `IDevice`
and the backend device IID return the same object; unrelated IIDs return `FE_NOINTERFACE`; reference
counts balance; the validation layer refuses the backend device IID.

### Shared build (`NVRHI_BUILD_SHARED=ON`)

- NVRHI linked. `qi_device` and `memory_queries` passed on D3D11 and Vulkan.
- The GTest core tests (`nvrhi_test_auto_ptr`, `nvrhi_test_qi_route`) do not link in this
  configuration: static versus dynamic CRT mismatch (see section 6).

### Runtime smoke runs

A first, short check during the work. These samples ran and exited with code 0; the sample sweep below
supersedes it.

| Sample | D3D11 | D3D12 | Vulkan |
| --- | --- | --- | --- |
| basic_triangle | Pass | Pass | Pass |
| meshlets | | Pass | Pass |
| rt_triangle | | Pass | Pass |
| async_compute | | Pass | Fails, pre-existing (section 6) |
| VXGISample | | Pass (normal run) | |

The VXGISample entry is a normal run. With the NVRHI validation layer on, VXGISample stops at a
validation error on every API; the sweep showed that this error is pre-existing (issue E in section 6).

### Final grep

Checked again when this record was written (read-only):

- No `RefCountPtr`, `RefCounter<`, `IResource`, `ResourceHandle`, `GetRefCount`, `donut/core/object` or
  `DONUT_IID` in NVRHI C++ sources, docs or natvis, or in donut C++ sources.
- **Exception:** `donut/tools/Gen-Interface.ps1` (a PowerShell snippet generator) still emitted
  `DONUT_IID`, `DONUT_DECLARE_UUID_TRAITS`, `DONUT_CCLSID` and `DONUT_DECLARE_INTERFACE_TABLE` when this
  record was written. Fixed afterwards in donut `d6ca69e` (section 6).
- In etherealsamples, the remaining `IResource` matches are GVDB's own `donut::gp::IResource`
  (`src/GVDBSamples/gvdb/GPDevice.h`), unrelated to NVRHI, and one `RefCountPtr` mention in a benchmark
  report (`benchmark/Asteroids/report/data/hotspots.md`) describing an older measurement.
- std smart pointers: none in `src/`. Text matches remain only in comments: the top of
  `include/nvrhi/core/autoptr.h`, one line in `include/nvrhi/core/foundation.h`, and one line in
  `tests/core/test_auto_ptr.cpp`.

### Sample sweep

Run on 2026-10-02, after the last commit in the table above.

**Method.**

- Clean Release rebuild of the aggregate: 666 of 666 build steps. Also compiled once with
  `DONUT_WITH_AUDIO=ON`. `AudioEngine.cpp` compiles in both configurations: `donut-engine.cmake` globs
  `src/engine/*.cpp`, so the file is built even with audio OFF.
- Each sample ran on every API it supports (`-dx11`, `-dx12`, `-vk`) for 7-10 s and then received
  `WM_CLOSE`, so the shutdown path ran. `OutputDebugString` output was captured with a DBWIN monitor.
- Every sample was run twice: normally, and in validation mode with the NVRHI validation layer plus the
  D3D12 debug layer or the Vulkan validation layers. Most samples enable these only under
  `#ifdef _DEBUG`, so validation mode used temporary Release edits, which were reverted afterwards. Only
  DDGISample (`--debug`) and rt_bindless and rt_particles (`-debug`) have a runtime flag.
- Every failure was re-run on a pre-refactor build in a separate worktree: aggregate `347d6b0` (donut
  `8ec223c`, nvrhi `9eb1458`), which has `nvrhi::core` but not the interface rebase or the pointer
  changes.

**Overall result.** No crash, hang, live object at shutdown or new validation message is attributable
to the refactor. On D3D11 and D3D12, `ReportLiveObjects` printed nothing at shutdown. Every failure below
reproduces identically on the pre-refactor build.

"Pass" means the sample ran, closed and exited cleanly, with no validation errors in validation mode.
Letters refer to the issues in section 6. "n/s" means the sample does not support that API.

| Sample | D3D11 | D3D12 | Vulkan | Notes |
| --- | --- | --- | --- | --- |
| basic_triangle | Pass | Pass | Pass | Both modes. |
| vertex_buffer | Pass | Pass | Pass | Needs `media/` (M). |
| headless | Pass | Pass | Pass | Prints "Test PASSED". |
| deferred_shading | Pass | Pass | Pass; 11 layout warnings in validation mode (A) | Needs `media/` (M). |
| meshlets | n/s | Pass | Pass | |
| threaded_rendering | n/s | Pass | Pass | Needs `media/` (M). |
| bindless_rendering | n/s | Pass | Pass | Needs `media/` (M). |
| variable_shading | n/s | Pass | Pass | Needs `media/` (M). |
| rt_triangle | n/s | Pass | Pass | |
| rt_shadows | n/s | Pass | Pass | Needs `media/` (M). |
| rt_bindless | n/s | Pass | Pass | Validation via `-debug`. Needs `media/` (M). |
| rt_reflections | n/s | Pass | n/s | Needs `media/` (M). |
| shader_specializations | n/s | n/s | Pass | |
| async_compute | n/s | Pass | Device Removed (B) | D3D11: "CommandListLifetimeTracker is not supported by the D3D11 backend". |
| rt_particles | n/s | Crash 0xC0000005 (C) | Crash 0xC0000005 (C) | Needs `media/` (M). |
| DDGISample | n/s | Pass; 3 D3D12 errors in validation mode (D) | Pass; 31 Vulkan warnings in validation mode (D) | Needs `--config etherealsamples/assets/config/cornell.cfg.json`. |
| VXGISample | Normal run crashes 0xC0000005 (E); validation error (E) | Normal run passes; validation error (E) | Normal run passes with `--adapter=1` (F); validation error (E) | |
| work_graphs_d3d12 | n/s | Not run: the device has no work-graph support | n/s | Environment. |
| asteroids_nvrhi (`-threads 1`, `-threads 4`) | Pass | Pass | Pass | Benchmark. |

VXGISample: in validation mode every API stops at the same NVRHI validation error (E). Without
validation, D3D12 and Vulkan run (Vulkan only with `--adapter=1`), and the D3D11 Release build crashes for
the same underlying reason. The normal D3D12 run agrees with the earlier smoke run.

## 6. Known issues and follow-ups

### Failures not caused by the refactor

- **`memory_queries_d3d12`.** On the Windows 10 D3D12 runtime, `CreateCommittedResource` fails with
  `0x80070057` (E_INVALIDARG) for the acceleration-structure buffer, because NVRHI sets a resource flag
  for acceleration-structure buffers that this runtime rejects. Fails in the same way without the
  refactor. The ray-tracing samples work around it by loading a stable Agility SDK runtime
  (`ethereal_use_agility_sdk()` in etherealsamples `src/CMakeLists.txt`); the test does not.
All of the following reproduce identically on the pre-refactor build (aggregate `347d6b0`). Letters
match the sample sweep table in section 5.

- **(M) Missing `media/`.** deferred_shading, vertex_buffer, threaded_rendering, bindless_rendering,
  variable_shading, rt_shadows, rt_bindless, rt_reflections and rt_particles look for `<build>/media`
  (the parent of `bin`), and nothing in the repositories provides it. With temporary directory junctions
  to the Donut-Samples media and to glTF-Sample-Assets they run. Follow-up: provide or fetch the media,
  or document where to get it.
- **(A) Vulkan layout warning in deferred_shading.** In validation mode the Vulkan layers report 11
  times that a draw "expects VkImage [BlackDepthStencilTexture2DArray] in
  DEPTH_STENCIL_READ_ONLY_OPTIMAL, current layout SHADER_READ_ONLY_OPTIMAL". Non-fatal. Probably the
  depth-texture layout handling in NVRHI's Vulkan backend; not investigated.
- **(B) async_compute on Vulkan reports "Device Removed".** Compute work is recorded on a transfer-only
  queue family (family 1). Confirmed again in the sweep.
- **(C) rt_particles crashes (0xC0000005) on D3D12 and Vulkan.** Its `UserInterface` constructor calls
  `ImGui::GetIO()` (`src/rt_particles/rt_particles.cpp`, about line 782; the object is constructed at
  about line 886) before an ImGui context exists. In this donut fork the `ImGuiRenderPass` constructor
  is empty (`donut/src/app/ImGuiRenderPass.cpp`, about line 486) and the context is created in `Init()`
  (about line 497). A porting bug from upstream Donut-Samples.
- **(D) DDGISample validation messages.** D3D12 (3 errors): the barrier layout SHADER_RESOURCE does
  not match the expected UNORDERED_ACCESS on DDGIOuputTexture, RTAO Raw and RTAO Output. Vulkan (31
  warnings): `StorageImageArrayNonUniformIndexing` is not enabled, and the "Samplers" array has 3
  elements in SPIR-V but `descriptorCount` 1. DDGISample also needs
  `--config etherealsamples/assets/config/cornell.cfg.json`; without it, it stops with "Config file not
  available".
- **(E) VXGISample opens two immediate command lists at once.** `src/VXGISample/VXGISample.cpp` opens
  one (about line 239), then calls `SetVoxelizationParameters` (about line 266), which reaches
  `VoxelRenderer::AllocateResources` (`VoxelRenderer.cpp`, about line 39) and opens a second (about line
  80). The NVRHI validation layer reports "Two or more immediate command lists cannot be open at the
  same time" on every API. On D3D11 both lists share the one immediate context, which crashes the
  Release build (0xC0000005).
- **(F) VXGISample on Vulkan picks the wrong adapter.** It defaults to adapter 0 (on the test machine an
  AMD 780M iGPU), which is rejected because its `maxMultiviewViewCount` is 6 and the sample requires 15.
  `--adapter=1` (RTX 4050) works.
- **etherealsamples `README.md`** says VXGISample takes `--debug`; it does not (only DDGISample has
  `--debug`, and rt_bindless and rt_particles have `-debug`).
- **work_graphs_d3d12** was not run: the test device has no work-graph support (environment, not a
  bug).
- **donut `test_string_utils` and `test_console_interpreter`** fail on float parsing and formatting. It
  has not been established whether they failed before this work; nothing in the refactor touches that
  code, but this should be checked on the pre-refactor tree (donut `a8448f1`).
- **Debug builds of the samples do not link** (ShaderTool), so all validation used Release.
- **Shared build:** the GTest core tests do not link because of a static/dynamic CRT mismatch. The
  exact cause was not analyzed; note that the shared `nvrhi` target sets `MSVC_RUNTIME_LIBRARY` to the
  static runtime while `nvrhi_core` does not set it.

### Bugs found in donut

- **Double free in `src/engine/AudioEngine.cpp`** (`Xaudio2Implementation::create`, line 892):
  `return nvrhi::MonoPtr<Engine::Implementation>(result);` with `result` an lvalue
  `MonoPtr<Xaudio2Implementation>`. `MonoPtr`'s implicit `operator pointer()` plus its `explicit
  MonoPtr(pointer)` constructor create a second owner. The same code was present before the refactor
  (donut `a8448f1`, `donut::MonoPtr`), so the bug is latent, not a regression. The file is compiled even
  with audio OFF (`donut-engine.cmake` globs `src/engine/*.cpp`), but the code path runs only with
  audio enabled (`DONUT_WITH_AUDIO`, OFF by default). The sweep also compiled it with
  `DONUT_WITH_AUDIO=ON`. The fix, `std::move(result)`, uses the converting move constructor enabled by
  nvrhi `8f810ce`. It is applied in the donut working tree and **not committed** at the time of writing.
- **Bugs in donut's former `core/object` code**, fixed in the moved copy (nvrhi `9eb1458`; details in
  ADR 0001): inverted `WeakReferenceImpl::IsExpired`, the `QIT_` typo in `RouteMemberQueryInterface`,
  `std::hash<WeakPtr>`, the `MonoPtr<T[]>` constructors, the self-referencing `QITraits` aliases, and the
  const `CompressedPair::GetFirst`.

### Follow-ups

- Commit the `AudioEngine.cpp` fix in donut.
- ~~Update `donut/tools/Gen-Interface.ps1` to emit `NVRHI_IID`, `NVRHI_DECLARE_UUID_TRAITS_DERIVED`,
  `NVRHI_CCLSID` and `NVRHI_DECLARE_INTERFACE_TABLE`.~~ Done in donut `d6ca69e` (nvrhi::core
  declarations). Since donut `20d2e3b` it emits `NVRHI_CLASS_CLSID` / `NVRHI_CLASS_INTERFACE_TABLE` for
  implementation classes (ADR 0006); since donut `b2f615c` plain traits and an explicit table (ADR 0007).
- Build once with `NVRHI_WITH_RTXMU=ON` to compile the converted RTXMU code.
- Consider making `MonoPtr::operator pointer()` explicit, to turn the `AudioEngine.cpp` class of bug
  into a compile error. This is an API change for donut.
- ~~Optionally replace `checked_cast` on `BindingSetItem::resourceHandle` with `QueryInterface`.~~
  Superseded by ADR 0006: every `checked_cast` now verifies through `QueryInterface` in Debug builds.
- Decide how the GTest core tests should link in the shared build (CRT choice).
- Fix the pre-existing sample issues M, A, C, D, E and F (section 6), and correct the VXGISample
  `--debug` statement in etherealsamples `README.md`.
- Give the samples a runtime switch for the validation and debug layers; most enable them only under
  `#ifdef _DEBUG`, and Debug builds do not link.
- Push the branches when asked.

### Follow-up: no RTTI (ADR 0006)

Done after this plan, on 2026-10-02: NVRHI no longer uses `dynamic_cast` or `typeid` and is compiled with
`/GR-` / `-fno-rtti`. `checked_cast` verifies through `QueryInterface` in Debug builds, without adding a
reference to an object that is being destroyed. Every backend implementation class has a class ID, and
the validation layer's type tests query the wrappers' class IDs. Decision, design and verification:
[ADR 0006](../adr/0006-no-rtti-queryinterface-casts.md) (nvrhi `d089ee4`, `6cfe4ad`; donut `20d2e3b`;
aggregate root `0514502`).

### Follow-up: explicit QueryInterface tables (ADR 0007)

Done on 2026-10-02, reversing step 3c (interface chains, ADR 0003): the implicit chain and route-parent
`QueryInterface` is gone. `ObjectImpl` and its layers implement no `QueryInterface`; every concrete class
writes an explicit table with all its interfaces, ancestors included, and its class ID (75 backend
classes, 47 donut and 12 etherealsamples tables; derived classes inherit their base's table). The
explicit `NVRHI_IMPLEMENTS_ROUTE_PARENT` / `NVRHI_IMPLEMENTS_ROUTE_MEMBER` entries, the pass-through layer
and the liveness probe stay; the probe is answered by the end of each table. A script audit of all 265
object-model classes and the extended `tests/qi-device.cpp` (exact IID and class-ID sets per object,
also through the validation layer) found no class answering fewer or more than its inheritance graph.
`donut/tools/Gen-Interface.ps1` emits the explicit form. Decision, audit and verification:
[ADR 0007](../adr/0007-explicit-queryinterface-tables.md) and its appendix (nvrhi `5922252`;
donut `b2f615c`, `ffc6c24`; etherealsamples `2d53121`; aggregate root `818eda4`, `ece3cfa`). The `AudioEngine.cpp` fix is still
uncommitted.

Follow-up, per-class check (ADR 0007, nvrhi `9990dba`, donut `3a36fb8`, etherealsamples `54a4610`, root `5396e23`): `MakeNewRCObj` now rejects a class that does not state its table in its own body; the 77 donut and sample classes that reuse a base's table say so with `NVRHI_INHERIT_INTERFACE_TABLE()`.

Follow-up, flat base classes (ADR 0007, nvrhi `e8a7951`, donut `a0b7909`, root `d0257db`): the root/pass-through layers and `QITraits` are gone; `ObjectImpl<Bases...>` and the other three base classes derive directly from their interfaces and mixins, reject an implementation class as a base (`details::IsObjectImpl`), and a class that builds on an implementation derives from it directly (only test fixtures had to change).

Follow-up, lowercase core headers: `include/nvrhi/core/{AutoPtr,DataBlob,Foundation,Memory,Threading,Types}.h` were renamed to `autoptr.h`, `datablob.h`, `foundation.h`, `memory.h`, `threading.h`, `types.h` (NVRHI file naming: lowercase, no separators), with every include and mention updated in nvrhi, donut and etherealsamples.
