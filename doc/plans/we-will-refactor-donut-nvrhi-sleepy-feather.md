# nvrhi: ABI-stable public headers for a shared-library build

## Context

nvrhi (`donut/nvrhi`, its own git repo inside the donut submodule) should build as a CMake SHARED library
(`/MDd` and `/MD` on MSVC). Today its public API passes `std::string` / `std::vector` / `std::optional`
across the boundary:
- 12 `std::string` fields and 7 `std::vector` fields in desc structs.
- `coopvec::DeviceFeatures`, which `queryCoopVecFeatures()` returns by value.
- `std::optional<ComponentMapping>` parameters.
- Classes exported as mangled C++ symbols (`NVRHI_API`).

STL layouts differ across compilers, and on MSVC between debug and release. The goal is public headers with no
STL containers, nvrhi-owned containers with a fixed layout, and a DLL that exports only `extern "C"` symbols.

**Decisions made with you:**
- **Containers:** a self-contained, header-only port modelled on EASTL (`D:\ps\repo\EASTL`, used as a reference
  only, with no build dependency).
- **Allocator:** stateful. Each container holds an `IMemoryAllocator*` and frees through it, so it stays safe across
  CRTs and compilers.
- **Build:** `NVRHI_BUILD_SHARED=ON` must work (DLL built with `/MD[d]`). The ethereal default stays static nvrhi,
  and the tree stays on `/MT`.
- **Exports:** only pure C function symbols are exported.
  - Free functions become `extern "C" nvrhiXxx()` exports, and the existing C++ name stays as an `inline` header
    wrapper.
  - C++ member functions, constructors and destructors are never exported. Their implementations move into the
    headers as `inline` code (step 6).

- **Aftermath:** in scope. `common/aftermath.h` becomes STL-free COM interfaces, and its implementation and STL
  details move into nvrhi `src` (step 5b).

- **Allocator mechanics:** exactly EASTL's. The copy constructor copies the source's allocator.
- **Inline-buffer vectors:** `fixed_vector` only. `cached_vector` is dropped, and `static_vector` stays for the
  existing fixed-capacity fields.
- **ABI scope:** within each ABI family (MSVC + clang-cl; GCC + Clang with libstdc++ or libc++) *and* between
  MSVC and MinGW. The interface rules in step 5c cover the MSVC ↔ MinGW part.
- **Exceptions:** every C export and every public interface method is `noexcept`. An exception escaping across
  the boundary terminates the program instead of being undefined behaviour.
- **`nvrhi_core` as a DLL** (step 5f): it owns the single process-wide default allocator and the `IDataBlob`
  implementations. It has its own option, `NVRHI_CORE_BUILD_SHARED` (default ON). nvrhi and every other COM
  module link it. The object-model templates stay header-only.

## Steps

### 1. Export macros: new `include/nvrhi/common/export.h`
- **`NVRHI_API`:**
  - `__declspec(dllexport)` when `NVRHI_SHARED_LIBRARY_BUILD`.
  - `__declspec(dllimport)` when `NVRHI_SHARED_LIBRARY_INCLUDE`.
  - `__attribute__((visibility("default")))` on GCC/Clang.
  - Empty in a static build.
- **`NVRHI_EXTERN_C`** (`extern "C"`) and **`NVRHI_C_API`** (`NVRHI_EXTERN_C NVRHI_API`).
- Delete the old definition at `nvrhi.h:47-64`.
- **Rule for every C export:** it takes and returns only scalars, enums and pointers. Aggregates go through
  out-pointers, so clang never warns `-Wreturn-type-c-linkage`.

### 2. `include/nvrhi/common/containers.h`: the portable containers
Allowed includes: `<cstddef> <cstdint> <cstring> <new> <type_traits> <utility> <initializer_list>` and
`nvrhi/core/memory.h` (for `IMemoryAllocator`, `GetDefaultMemAllocator()` and `NVRHI_ASSERT`).

**Layout rules:**
- No exceptions: out-of-memory goes through `NVRHI_VERIFY`.
- No debug-only members.
- No `[[no_unique_address]]`.
- `static_assert` the `sizeof` of each type in the header.

**Allocator model** (EASTL mechanics, from `EASTL/vector.h` 586-680, 1407-1436, 487-492):
- The allocator is a container template parameter, stored by value next to the capacity pointer.
- Containers expose `get_allocator()` / `set_allocator()`. `set_allocator` asserts if memory is already held.
- Move-construction moves the allocator and steals the buffer.
- Move-assignment and `swap` swap pointers only when the allocators compare equal. Otherwise they move element by
  element into storage from each container's own allocator.
- Copy-assignment never changes the allocator (EASTL `EASTL_ALLOCATOR_COPY_ENABLED=0`).
- Copy-construction copies the source's allocator, exactly as EASTL does (`vector.h:586`). For default-allocated
  containers that is the shared `nvrhi_core` allocator (step 5f), so a client copy of a desc returned by the DLL
  frees into the same heap. A custom `IMemoryAllocator` must outlive every container that copied it.

**`nvrhi::allocator`** (the default):
- Holds `{ IMemoryAllocator* impl; }`, which defaults to `GetDefaultMemAllocator()`. With `nvrhi_core` as a DLL
  (step 5f), that is the single process-wide allocator owned by `nvrhi_core.dll`.
- `operator==` compares `impl`. Default-allocated containers compare equal in every module, so EASTL's
  move-assign and swap simply swap buffers, even between nvrhi.dll and a client. That is correct because there is
  only one heap. Containers built with a custom `IMemoryAllocator` compare unequal and fall back to moving
  elements one by one.
- `allocate(bytes, align)` uses `AllocateAligned` when `align > alignof(max_align_t)`, else `Allocate`;
  `deallocate` matches.

**`vector<T, Allocator = allocator>`** (reference: EASTL `vector.h`):
- Layout `{T* begin; T* end; T* capEnd; Allocator}` (32 bytes on x64). Iterators are raw pointers. Works for
  non-trivial `T`.
- Full std-style subset:
  - Constructors: default, count, count + value, iterator range, `initializer_list`, copy and move.
  - `assign`, `operator=` (including `initializer_list`).
  - `begin/end/cbegin/cend`, `size/empty/capacity/reserve/resize/shrink_to_fit/clear`.
  - `data/operator[]/at/front/back` (with an assert).
  - `push_back/emplace_back/pop_back`.
  - `insert` (position + value, range, `initializer_list`; `ViewTracer.cpp` needs `insert(end(), {...})`).
  - `erase` (one element, range), `swap`, `==` / `!=`.

**`string`** (reference: EASTL `string.h`, char only):
- 24-byte SSO union (23 chars inline, heap flag in the top bit of the last byte), plus the allocator (32 bytes).
- Constructors and assignment from `const char*`, `(const char*, n)`, copy and move.
- A templated implicit constructor, `operator=`, `append` and comparison for any *StringLike* type with
  `data()` + `size()`. This accepts `std::string` / `std::string_view` without including them.
- Operations:
  - `c_str/data/size/length/empty/capacity/reserve/resize/clear`, `begin/end`, `operator[]`.
  - `append`, `+=`, `+` (string or `const char*` on either side).
  - `compare`, `==` / `!=` / `<` against `string` and `const char*`, `find/rfind/substr`.
- A template `operator<<(OStream&, const string&)` constrained on `os.write(...)`.

**`fixed_vector<T, N, bEnableOverflow = true, OverflowAllocator = allocator>`** (reference: EASTL `fixed_vector.h`):
- Has the vector interface, with an inline aligned buffer of N elements.
- With overflow on, it grows into the heap. With overflow off, it asserts when full.
- Adds `full()`, `has_overflowed()` and `max_size()`.
- Copy and move re-point to their own inline buffer.


**`static_vector<T, N>`:**
- Rewrite without `std::array` as `T m_elements[N]; size_t m_size;`.
- Keep the current API and semantics, because it is used in hot paths (`BindingSetVector`, `ViewportState`, ...).

### 3. Move `salieri.h` and remove `std::optional`
- Move `donut/include/donut/core/salieri.h` to `donut/nvrhi/include/nvrhi/core/salieri.h`: add it in the nvrhi
  repo and `git rm` it in donut. Update its only user, `ethereal-samples/src/VXGISample/ViewTracer.h:7`, to
  `<nvrhi/core/salieri.h>`. `nvrhi.h` includes it.
- Replace `std::optional<ComponentMapping>` with `_In_opt_ const ComponentMapping*`:
  - `packComponentMapping` (`nvrhi.h:473`), plus a `const ComponentMapping&` overload.
  - `resolveComponentMapping` (`:487`).
  - `ITexture::getNativeView(..., _In_opt_ const ComponentMapping* overrideComponentMapping = nullptr)`
    (`:644`), and its overrides in `src/d3d11/d3d11-backend.h:95`, `src/d3d12/d3d12-backend.h:355`,
    `src/vulkan/vulkan-backend.h:489` and the `*-texture.cpp` implementations.
  - `BindingSetItem::Texture_SRV` (`:2267`) and `setOverrideComponentMapping` (`:2491`), each with a
    by-reference overload.
- Annotate the other defaulted `= nullptr` pointer parameters in the public headers with
  `_In_opt_` / `_Out_opt_`.
- Downstream: `donut/include/donut/engine/TextureCache.h:66` becomes `packComponentMapping(opt ? &*opt : nullptr)`.

### 4. Convert desc fields in `nvrhi.h` / `vulkan.h`
- **`std::string` → `nvrhi::string`:**
  - `debugName` (`HeapDesc`, `TextureDesc`, `BufferDesc`, `ShaderDesc`, `rt::OpacityMicromapDesc`,
    `rt::AccelStructDesc`, `rt::ShaderTableDesc`).
  - `name` (`VertexAttributeDesc`, `CustomSemantic`), `entryName`, `exportName` ×2.
  - `vulkan::DeviceDesc::vulkanLibraryName`.
  - Setters take `const string&`. The StringLike constructor keeps `setDebugName(std::string)` and string
    literals working.
- **`std::vector` → `nvrhi::vector`:**
  - `OpacityMicromapDesc::counts`, `AccelStructDesc::bottomLevelGeometries`.
  - `BindingLayoutDesc::bindings`, `BindingSetDesc::bindings`.
  - `rt::PipelineDesc::shaders/hitGroups`, `coopvec::DeviceFeatures::matMulFormats`.
  - `setCounts` takes `const vector<...>&`.
- Bump `NVRHI_HEADER_VERSION` (`src/common/versioning.h`), because every desc layout changes.

### 5. Clean up the public header includes
- **`nvrhi.h`:**
  - Include `core/types.h`, `core/autoptr.h`, `core/salieri.h`, `common/export.h`, `common/containers.h`,
    `common/resource.h` and `nvrhiHLSL.h`.
  - Drop `core/foundation.h`, `<optional>`, `<string>` and `<vector>`.
  - Keep `<cstdint> <cmath> <cstring>`.
- **`common/resource.h`:**
  - Include only `core/types.h` (`IObject`, `NVRHI_IID`, `NVRHI_DECLARE_UUID_TRAITS`).
  - Forward-declare `template <typename T> class AutoPtr;` for the `RHIObjectHandle` typedef.
- **`core/autoptr.h`:** unchanged.
  - It keeps `<memory>` (for `std::addressof`) and `<functional>`, plus its `std::hash` specializations for
    `AutoPtr`/`WeakPtr`/`MonoPtr`.
  - This is the only public header allowed to include `<memory>`. No public header uses
    `std::shared_ptr` / `unique_ptr` / `weak_ptr`.
- **`std::hash` stays in place:**
  - `hash_combine` and the specializations at `nvrhi.h:3972-4091` stay where they are. `<functional>` arrives
    through `autoptr.h`.
  - `std::hash<nvrhi::string>` is added in `containers.h`, which includes `<functional>`.
  - No new hash headers, and no include changes for hash users downstream.
- **`core/datablob.h`:** the blob implementation classes move into `nvrhi_core` (step 5f). The header keeps only
  the inline `CreateBlob` / `CreateStringBlob` / `CreateProxyBlob` / `CreateProxyBlobFromSource` wrappers, with
  unchanged signatures, so its 14 callers need no changes. It no longer includes `foundation.h`, `<string>` or
  `<vector>`.
- **`utils.h`:**
  - Drop `<mutex>`.
  - Move `BitSetAllocator` to `src/common/bitset-allocator.h`, keeping its implementation in `utils.cpp`.
  - Move the unexported internal helpers (`Generate{Heap,Texture,Buffer}DebugName`, `NotImplemented`,
    `NotSupported`, `InvalidEnum`, `DebugNameToString`) to a new `src/common/utils-internal.h`. They are only
    used inside nvrhi `src`.
- **`common/resourcebindingmap.h`:**
  - Move to `src/common/resourcebindingmap.h`.
  - Update `src/d3d11/d3d11-backend.h` and `src/d3d12/d3d12-backend.h`.
  - Remove it from `include_common` in `CMakeLists.txt`.
- **Not changed:** `core/foundation.h` and `core/threading.h` stay as implementation-side headers; nothing in
  the public API chain reaches them any more.

### 5b. Aftermath: COM interfaces in public, implementation in `src`
Today `common/aftermath.h` exposes two concrete classes that use `std::filesystem::path`, `std::array`,
`std::unordered_map`, `std::set`, `std::deque` and `std::function`. `IDevice::getAftermathCrashDumpHelper()`
returns one of them by reference. The only external user is donut:
- `src/engine/ShaderFactory.cpp:66,74`: register/unregister with
  `std::bind(&ShaderFactory::FindShaderFromHash, ...)`.
- `src/app/aftermath/AftermathCrashDump.cpp:66-67,209-212`: `findShaderBinary` and `ResolveMarker`.

**New public `include/nvrhi/common/aftermath.h`**, which includes only `nvrhi.h`:
```cpp
// Stateless; donut's AftermathCrashDump::GetShaderHashForBinary is already static.
typedef uint64_t (*PFN_AftermathShaderHashGenerator)(const void* binary, size_t size, GraphicsAPI api);

NVRHI_IID(IAftermathShaderBinaryLookup, "<new uuid>")        // implemented by clients (donut ShaderFactory)
struct IAftermathShaderBinaryLookup : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IAftermathShaderBinaryLookup)
    virtual bool findShaderBinary(uint64_t shaderHash, PFN_AftermathShaderHashGenerator hashGenerator,
                                  const void*& outBinary, size_t& outSize) noexcept = 0;
};

NVRHI_IID(IAftermathCrashDumpHelper, "<new uuid>")           // implemented by nvrhi, one per device
struct IAftermathCrashDumpHelper : IObject {
    NVRHI_DECLARE_UUID_TRAITS(IAftermathCrashDumpHelper)
    // Non-owning, like today's void* client key: the client unregisters before it dies (no ref cycle).
    virtual void registerShaderBinaryLookup(IAftermathShaderBinaryLookup* lookup) noexcept = 0;
    virtual void unregisterShaderBinaryLookup(IAftermathShaderBinaryLookup* lookup) noexcept = 0;
    // The string stays owned by nvrhi and is valid until the device is destroyed (same lifetime as today's reference).
    virtual bool resolveMarker(uint64_t markerHash, const char*& outString, size_t& outLength) noexcept = 0;
    virtual bool findShaderBinary(uint64_t shaderHash, PFN_AftermathShaderHashGenerator hashGenerator,
                                  const void*& outBinary, size_t& outSize) noexcept = 0;
};
```
- **`IDevice::getAftermathCrashDumpHelper()`** (`nvrhi.h:3961`) returns `IAftermathCrashDumpHelper*`, not
  AddRef'd, and `nullptr` when Aftermath is off. Update the overrides in `src/d3d12/d3d12-backend.h:1580`,
  `src/vulkan/vulkan-backend.h:1413` and `src/validation/validation-backend.h:450` / `validation-device.cpp:2367`.
  The `ResolvedMarker`, `BinaryBlob`, `ShaderHashGeneratorFunction` and `ShaderBinaryLookupCallback` typedefs are
  removed.

**New `src/common/aftermath.h`** (internal, STL allowed):
- `AftermathMarkerTracker` moves here unchanged. It is used only by the d3d12 and vulkan command lists.
- `AftermathCrashDumpHelper` becomes an `ObjectImpl` that implements `IAftermathCrashDumpHelper`, keeping today's
  `std::set` / `std::deque` / `unordered_map` members. The lookup map is keyed by the interface pointer.
- **`src/common/aftermath.cpp`** keeps its logic. `ResolveMarker` / `findShaderBinary` adapt to the out-pointers.
- **Devices:** `src/d3d12/d3d12-backend.h:1637` and `src/vulkan/vulkan-backend.h:1425` hold an
  `AutoPtr<AftermathCrashDumpHelper>` created in the device constructor. It stays the first member, keeping the
  destruction-order rule in `vulkan-backend.h:1422`. The command lists'
  `registerAftermathMarkerTracker` / `unRegisterAftermathMarkerTracker` calls go to the concrete type.
- **CMake:** move `common/aftermath.h` from `include_common` to the src list, and add the new public header.
  Aftermath stays allowed in the shared build (drop the `FATAL_ERROR` idea).

**Donut changes:**
- **`ShaderFactory`:** owns a small `MAKE_RC_OBJ` `IAftermathShaderBinaryLookup` with a back-pointer.
  - It registers in the constructor and unregisters in the destructor.
  - `FindShaderFromHash` (`include/donut/engine/ShaderFactory.h:153`) takes `PFN_AftermathShaderHashGenerator`
    instead of `std::function`.
- **`AftermathCrashDump`:**
  - `GetShaderHashForBinary(const void*, size_t, GraphicsAPI)`.
  - `ResolveMarker` returns `const char*` plus a length; `ResolveMarkerCallback` uses them directly.
  - The `findShaderBinary` call site uses the out-pointers.

### 5c. Interface rules so MSVC and MinGW agree (COM-style)
A scan of all public headers except `foundation.h` found four kinds of code that MSVC (and clang-cl) compile
differently from MinGW GCC/Clang. Every public interface (`I*` struct) must follow these rules:

**R1. Virtual functions follow the D3D12 return protocol: they never return a class, struct or union by value.**
- **The problem:** for instance methods, MSVC passes the hidden return pointer *after* `this`, while MinGW passes
  it before. MSVC also returns *every* user-defined type (struct, class or union, even 8 bytes) through that hidden
  pointer, where MinGW uses RAX. That is why turning `Object` into a union would not help.
- **Methods that produce interfaces** use `virtual FRESULT <method>(<params>, IXxx** pp<Name>) noexcept = 0;`.
  - They return `FS_OK` with a +1 reference, or an `FE_*` code with `*pp = nullptr`.
  - The method names stay the same, and there are **no inline convenience overloads** (pure D3D12 style).
  - **Affected:**
    - `IDevice`: every `create*` and `createHandleForNative*`.
    - `ISamplerFeedbackTexture::getPairedTexture`, `IShaderLibrary::getShader`, `rt::IPipeline::createShaderTable`.
    - `d3d12::IDevice`: `buildRootSignature`, `createHandleForNative{Graphics,Meshlet}Pipeline`.
  - Example: `virtual FRESULT createTexture(const TextureDesc& desc, ITexture** ppTexture) noexcept = 0;`
- **Native objects:**
  - `struct Object` is removed and replaced by `using NativeObject = void*;` (namespace `nvrhi`), with
    `static_assert(sizeof(void*) == 8)`. Vulkan non-dispatchable handles fit only on 64-bit, and ethereal is x64
    only.
  - `getNativeObject`, `getNativeView` and `getNativeQueue` keep their names and return `NativeObject`, a scalar
    pointer.
  - `createHandleForNativeTexture`, `createHandleForNativeBuffer` and `createSamplerFeedbackForNativeTexture` take a
    `NativeObject`.
  - **Call sites:** the 80 downstream `getNative*` calls relied on `Object`'s implicit `operator T*()`, so they gain
    a `static_cast<ID3D12Device*>(...)` or `static_cast<VkImage>(...)`. nvrhi `src` changes the same way.
- **Plain-struct results** use the WIDL form that `d3d12.h` uses for non-MSVC compilers, with C++ references in
  place of the pointers: `virtual T& <method>(T& retVal, <params>) noexcept = 0;`.
  - The caller passes the storage, and the method fills it and returns `retVal`.
  - A reference is passed and returned exactly like a pointer on MSVC x64, Win64 MinGW and SysV, so this has the
    same ABI as WIDL's `T* f(T* RetVal, ...)`.
  - **Affected:**
    - `MemoryRequirements& get{Texture,Buffer,AccelStruct}MemoryRequirements(MemoryRequirements& retVal, ...)`.
    - `rt::cluster::OperationSizeInfo& getClusterOperationSizeInfo(OperationSizeInfo& retVal, ...)`.
    - `coopvec::DeviceFeatures& queryCoopVecFeatures(coopvec::DeviceFeatures& retVal)`. The caller's
      `nvrhi::vector` keeps its own allocator, because assignment never moves allocators.
    - `queryCoopVecMatMulFormatSupport` / `queryCoopVecTrainingFormatSupport(…& retVal, ...)`.
    - `d3d12::IDescriptorHeap::getCpuHandle` / `getCpuHandleShaderVisible` / `getGpuHandle(
      D3D12_{CPU,GPU}_DESCRIPTOR_HANDLE& retVal, DescriptorIndex)`.
  - **Call sites** declare the result first: `nvrhi::MemoryRequirements req; device->getTextureMemoryRequirements(req, tex);`.
- **Reference vs pointer for other parameters:**
  - Mandatory outputs and inputs use C++ references, e.g. the Aftermath interface in step 5b becomes
    `findShaderBinary(..., const void*& outBinary, size_t& outSize)` and
    `resolveMarker(uint64_t, const char*& outString, size_t& outLength)`.
  - Optional parameters stay pointers annotated `_In_opt_`/`_Out_opt_` (salieri), since a reference cannot be
    null. Examples: `getNativeView`'s `const ComponentMapping*`, and `IRHIObject::queryMemoryRequirements(
    MemoryRequirements&)`, which already uses a reference.
  - Interface outputs stay `IXxx**`, as you specified. That keeps `device->createTexture(desc, &handle)` working
    through `AutoPtrRef`.
- **No class types by value in virtual parameters either.**
  - `TextureSubresourceSet` (16 bytes, trivially copyable) is passed by value to `ITexture::getNativeView`,
    `ICommandList::clearTextureFloat`, `clearDepthStencilTexture`, `clearTextureUInt`,
    `beginTrackingTextureState` and `setTextureState`.
  - It happens to be safe today: Win64 passes it through a pointer to a caller-made copy under both MSVC and
    MinGW, and SysV compilers agree.
  - It still becomes `const TextureSubresourceSet&`, so the rule (and the lint) can be simply "no class, struct or
    union by value anywhere in a virtual signature". Call sites don't change, because temporaries bind to
    `const&`.
  - `Object` parameters become `NativeObject` (see above).
- **Not affected:** Vulkan handles (`vulkan.h` `IDevice::getQueueSemaphore` / `queueWaitForSemaphore` /
  `queueSignalSemaphore` with `VkSemaphore`). Non-dispatchable handles are opaque pointers on 64-bit
  (`uint64_t` on 32-bit), so they are scalars.
- **Unchanged:** virtuals that already return a raw interface pointer (e.g. `ICommandList::getDevice()`,
  `d3d12::IDevice::getDescriptorHeap()`, `IDevice::getAftermathCrashDumpHelper()`) or `const T&`
  (`getDesc()`, `getFramebufferInfo()`) are ABI-safe and stay as they are.
- **Backends:** d3d11, d3d12, vulkan and the validation wrapper implement the new signatures.
- **Downstream call sites** in donut, ethereal-samples (including the Asteroids benchmark) and nvrhi `tests/`
  migrate to the out-pointer form, about 584 `create*` calls plus the getters:
  `nvrhi::TextureHandle tex; device->createTexture(desc, &tex);`. `&handle` already converts to `ITexture**`
  through `AutoPtrRef`, releasing any old value first. Code that ignored failures may check the `FRESULT`.
- **The C exports follow the same protocol:**
  `FRESULT nvrhiD3D12CreateDevice(const d3d12::DeviceDesc*, d3d12::IDevice** ppDevice) noexcept`, and likewise for
  d3d11, vulkan and `nvrhiCreateValidationLayer`.
- **`FRESULT` codes:** fix `core/types.h:203-204`, where `FE_INVALID_ARGS` and `FE_NOT_ALIVE_OBJECT` are both
  `-4`. Renumber `FE_NOT_ALIVE_OBJECT` and the codes after it so every code is distinct, and add
  `FE_OUT_OF_MEMORY` and `FE_UNSUPPORTED` for the create paths.

**R2. No virtual destructors in interfaces; interfaces are COM objects.** MSVC gives a virtual destructor one
vtable slot and MinGW two, so every later method shifts.
- **`IMessageCallback`** (`nvrhi.h:3254`) derives from `IRHIObject`.
  - It gets a new `NVRHI_IID`, and its virtual destructor, protected constructor and deleted copies are removed.
    `message(...)` becomes `noexcept`.
  - Devices hold the callback through an `AutoPtr` (AddRef'd) instead of a raw pointer.
  - In donut, `DefaultMessageCallback` (`DeviceManager.h:92`, `DeviceManager.cpp:1207`) becomes an
    `ObjectImpl<nvrhi::IMessageCallback>` with an interface table. `GetInstance()` hands out a function-local
    static `AutoPtr` created with `MAKE_RC_OBJ`. The `DeviceManager_DX11.cpp:240` / `DX12.cpp:312` uses take
    `.Get()`.
  - The GVDB sample's own `IMessageCallback` (`GPDevice.h:288`) is a different type and is unaffected.
- **`d3d12::IDescriptorHeap`** (`d3d12.h:68`) derives from `IRHIObject`, gets a new `NVRHI_IID`, and loses its
  virtual destructor and protected constructor.
  - `StaticDescriptorHeap` (`d3d12-backend.h:173`) becomes an `ObjectImpl<IDescriptorHeap>` with an interface
    table, created with `MAKE_RC_OBJ`. The device holds it in an `AutoPtr`.
  - `getDescriptorHeap()` still returns a non-owning pointer.
- The ABI lint fails on any `virtual ~` in an interface.

**R3. No overloaded virtual functions.** MSVC groups overloads together in reverse declaration order, so the
vtable slots differ. Overloads get numbered suffixes in declaration order, with no inline wrapper under the old
name:
- **`ICommandList::copyTexture`** (`nvrhi.h:3387-3395`):
  - `copyTexture1(ITexture*, …, ITexture*, …)`
  - `copyTexture2(IStagingTexture*, …, ITexture*, …)`
  - `copyTexture3(ITexture*, …, IStagingTexture*, …)`
- **`IDevice::createGraphicsPipeline`** (`:3893,3896`):
  - `createGraphicsPipeline1(desc, const FramebufferInfo&, IGraphicsPipeline**)`
  - `createGraphicsPipeline2(desc, IFramebuffer*, IGraphicsPipeline**)`
- **`IDevice::createMeshletPipeline`** (`:3900,3903`): `createMeshletPipeline1` / `createMeshletPipeline2`, split
  the same way.
- Downstream call sites are renamed.

**R4. No inheritance between data structs that cross the boundary.**
- **The problem:** MinGW places derived members inside a non-POD base's tail padding; MSVC does not.
  `FramebufferInfoEx : FramebufferInfo` (`nvrhi.h:1450`) puts `width` at offset 28 under MinGW and 32 under MSVC.
  It reaches clients through `IFramebuffer::getFramebufferInfo()`.
- **Fix:** `FramebufferInfoEx` no longer derives from `FramebufferInfo`.
  - It declares its own copies of `colorFormats`, `depthFormat`, `sampleCount` and `sampleQuality`, followed by
    `width`, `height` and `arraySize`, with the same builders and `operator==`.
  - An inline `FramebufferInfo getInfo() const` converts it explicitly. Code that passed an `Ex` where a
    `FramebufferInfo` was expected (e.g. `BloomPass.cpp:142` into `createGraphicsPipeline1`) calls `.getInfo()`.
  - The inline `FramebufferInfoEx(const FramebufferDesc&)` constructor (step 6a) builds a `FramebufferInfo` and
    copies its fields.
  - The `std::hash<FramebufferInfo>` users are unaffected.
- `static_assert`s on `sizeof` / `offsetof` for `FramebufferInfo`, `FramebufferInfoEx` and every desc struct that
  crosses the boundary.
- The ABI lint fails on any non-interface struct in a public header that has a base class (containers excepted).

**R5. `noexcept` at the boundary.**
- Every C export is declared `noexcept`, and so is every virtual in the public interfaces, including the
  client-implemented callbacks: `IMessageCallback::message`, `IAftermathShaderBinaryLookup`, and `IObject`
  (`IDataBlob`, `IWeakReference*`, `IMemoryAllocator`).
- Overriders must add `noexcept` too. That covers all nvrhi backends, `core/foundation.h` (`ObjectImpl` and the
  others), the `core/datablob.h` implementations, and donut's `StbImageBlob` (`TextureCache.cpp:85`) and
  `DefaultMessageCallback` (`DeviceManager.h:92`).

**Already safe** (no change needed):
- Bitfields: `FormatInfo`, `rt::InstanceDesc`, `BindingLayoutItem`, `BindingSetItem` and `nvrhiHLSL.h`.
  Adjacent fields have types of the same size, so both compilers produce the same layout.
- Types: `GUID`, `F*` types are all `int32_t`, and there is no `long`/`wchar_t`/`alignas`/`pack`/pointer-to-member.
- Enums: every enum has a fixed underlying type except `nvrhi.h:910` (`int` everywhere). I'll pin it to
  `: uint32_t` anyway.
- No `_DEBUG`-dependent layouts.
- Structs passed by value (`TextureSubresourceSet`) are trivially copyable, so Win64 passes them the same way on
  both. `NativeObject` is a plain pointer.

### 5d. `core/foundation.h` across DLL boundaries
`foundation.h` is the header-only toolkit (`ObjectImpl`, `WeakReferenceSourceImpl`, `Delegating*Impl`,
`MakeNewRCObj`/`MAKE_RC_OBJ`, interface tables) that nvrhi and other modules (donut, samples, future DLLs) use to
implement COM interfaces. Every module instantiates its own copy. It is safe across a DLL boundary only while the
other side touches an object **through its interface vtables alone**.

**Already safe:**
- **Destruction and freeing stay in the module that created the object.**
  - `ObjectImpl::Release` and `ReleaseStrongRef` → `DestroyObject` → `ObjectWrapper` / `PackedObjectWrapper`
    (`:423-470`).
  - That wrapper's vtable was set up by `MakeNewRCObj` in the creating module. So the destructor and
    `Free`/`delete` always run that module's code and allocator, whoever drops the last reference. This matches
    the `memory.h:94-97` rule.
- **`QueryInterface` tables** compare IIDs by value and are walked by code of the object's own module. They also
  work when every module has its own copy of `QIOffsetEntry` or `QIIIDOf`.
- **Vtables of the internal helper interfaces** follow R2 and R3: `ObjectWrapperBase` has 3 distinct virtuals, no
  destructor and no overloads. `WeakReferenceImpl`'s `IWeakReference` vtable has no destructor or overloads.
- **Virtual destructors in implementation classes** (`WeakReferenceSourceImpl:1062`) are added after the
  interface slots, so callers that use the interface never see them.

**Unsafe, must fix:**

| # | Issue | Fix |
|---|---|---|
| F1 | **`WeakPtr` compiles another module's control block into the caller.** `WeakRefTypeTrait` (`autoptr.h:162-172`) picks `T::WeakRefImplType` = `details::WeakReferenceImpl` whenever the concrete class is visible. `WeakReferenceImpl` is `final`, so the calling module devirtualizes and inlines **its own** `AddRef`, `Release`, `Resolve`→`QueryObject` (`SpinLock`, atomics) and `ReleaseWeakRef`→`SelfDestroy` against a control block laid out and allocated by the creating module. | `WeakPtr` always holds `IWeakReference*` and only makes virtual calls. Remove the `WeakRefImplType` path from `WeakRefTypeTrait`, and stop `WeakReferenceSourceImpl` from exporting `WeakRefImplType` (`:1117-1120`). The cost is one indirect call per weak `AddRef`/`Release`/`Lock`. |
| F2 | **`NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT` can be set per module** (`:17-19` `#ifndef`). Different values change `WeakReferenceSourceImpl`'s layout (inline `WeakReferenceImpl` vs pointer, `:1140-1148`). In unpacked mode, `SelfDestroy()` runs `delete this` (`:908`) through `UserAllocated::operator delete`, which uses the *running* module's `GetDefaultMemAllocator()`. If the last weak reference is dropped in another module (made possible by F1), memory is freed into the wrong heap. | **No change (your decision).** It is still safe, for two reasons. After F1, every weak `Release` goes through the `IWeakReference` vtable, so `SelfDestroy` / `delete this` always runs in the creating module. And with `nvrhi_core.dll` (step 5f), `UserAllocated`'s `operator new`/`delete` use the single process-wide allocator anyway. The layout difference only matters if one module reaches into another module's concrete class, which F3 forbids. The macro therefore stays a per-module choice. |
| F3 | **Implementation classes depend on the compiler's layout.** For example, MSVC gives the non-first empty base `ObjectImplTag` (`:996`) storage, while Itanium ABIs (GCC, Clang, MinGW) give it none. Code that reaches into another module's concrete class breaks across compilers (and across header versions). Two places do this: `checked_cast`/`NVRHI_IMPLEMENTS_CLASS` CLSID queries that return an implementation pointer (`:162`, `misc.h:84`), and aggregation, where `RouteMemberQueryInterface` (`:407-414`) makes a qualified, non-virtual `NonDelegatingQueryInterface` call on the member's concrete type. | **Module-private rule:** class IDs, implementation classes and aggregation never cross a module. Other modules may hold only interface pointers. Document this in `foundation.h` and the README. `NVRHI_IMPLEMENTS_CLASS` stays for querying objects of the same module. No code change in nvrhi: its class IDs are only queried inside nvrhi. |
| F4 | **ELF symbol interposition.** Every `ObjectImpl<...>`, `MakeNewRCObj<...>`, interface-table `QueryInterface` and `GetDefaultMemAllocator` instantiation has vague linkage. On Linux, with default visibility, the dynamic linker merges the copy in `libnvrhi.so` with a same-named copy in the executable or another `.so`. If those copies were built differently (compiler, `NDEBUG`, header version), behaviour changes silently. Windows DLLs keep separate copies and are unaffected. | nvrhi already builds with hidden visibility (step 7). Also give `nvrhi_core` an INTERFACE `-fvisibility=hidden -fvisibility-inlines-hidden` on GCC/Clang, so every module that implements COM objects with `foundation.h` keeps its instantiations private. |
| F5 | **Exceptions.** `MakeNewRCObj` (`:1323-1427`) uses `try`/`catch`/`throw`, so a module built with `-fno-exceptions` cannot compile `foundation.h`. Under R5 (`noexcept` boundary), a constructor that throws inside a `create*` call now terminates instead of reaching the caller. | Guard the `try`/`catch` blocks with `#if defined(__cpp_exceptions) \|\| defined(_CPPUNWIND)`. In the R5 wording, document that `create*` implementations catch `std::bad_alloc` and similar and return `FE_OUT_OF_MEMORY` (or another `FE_*`) rather than letting it escape. |
| F6 | **R5 `noexcept`** must also be added to the overrides in `foundation.h`: `ObjectImpl`, `WeakReferenceSourceImpl` and `Delegating*Impl` (`AddRef`, `Release`, `QueryInterface`, `GetWeakReference`), `WeakReferenceImpl` (`Resolve`, `GetNumStrongRefs`, `IsExpired`), the `ObjectWrapperBase` virtuals, and the macro-generated table `QueryInterface`/`NonDelegatingQueryInterface` (`:108-131`, `:195-200`). | As stated. |

### 5e. The other core helpers (`types.h`, `memory.h`, `threading.h`, `autoptr.h`, `datablob.h`, `misc.h`)

**Already safe:**
- `GUID` is 16 bytes (asserted in `src/core/core.cpp:35`) and `F*` types are `int32_t`.
- `AutoPtr` is one pointer (`core.cpp:40`), so `*Handle` members inside desc structs passed by `const&` keep a
  fixed layout, and AddRef/Release go through the vtable.
- The `IObject`, `IWeakReference`, `IWeakReferenceSource`, `IDataBlob` and `IMemoryAllocator` vtables have no
  destructors and no overloaded virtuals (R2/R3).
- The default allocator is reached through `IMemoryAllocator`'s vtable. After step 5f there is a single instance
  in `nvrhi_core.dll`.
- `CompressedPair` (`autoptr.h:95`) uses a single empty base, which MSVC and Itanium lay out the same way.

**Must fix:**

| # | Where | Problem | Fix |
|---|---|---|---|
| C1 | `types.h:82-134` `uuid_of` | **IIDs can differ between compilers.** On `__clang__` (including **clang-cl, which is in the MSVC family**), `uuid_of<T>()` is `T::this_uuid()` from `NVRHI_DECLARE_UUID_TRAITS`. Every other compiler uses the `NvrhiUUIDTraits<T>` specialization that `NVRHI_IID` emits. A derived interface or class that forgets `NVRHI_DECLARE_UUID_TRAITS` silently inherits its **base's** IID on clang, but gets its own on MSVC/GCC. A clang-cl module and an MSVC module would then disagree on `QueryInterface`. The same divergence exists for `NVRHI_CCLSID`/`NVRHI_SCLSID` (`foundation.h:21-39`). | **No change (your decision).** Known risk: always write `NVRHI_DECLARE_UUID_TRAITS` in every IID'd interface or class. |
| C2 | `types.h:25-58` GUID literal | **Case-sensitive, unchecked parsing.** `ascii_to_hex` handles lowercase hex only and validates nothing. An uppercase GUID string silently produces a different IID, so two modules spelling the same IID in different case cannot find each other's interfaces. | Accept `A-F`, reject any other character, and check the length (36) and dash positions in the `constexpr` parser. Invalid input becomes a compile error. Add `static_assert`s in `core.cpp` that uppercase and lowercase spellings produce the same value. |
| C3 | `types.h:203-204` | `FE_INVALID_ARGS` and `FE_NOT_ALIVE_OBJECT` are both `-4`. | Already in R1: renumber. |
| M1 | `memory.h:153-221` `STDAllocator` | **Claims allocators from different modules are equal.** `is_always_equal` is true for `DefaultMemoryAllocator`, and `operator==` returns true. But each module has its own instance on its own CRT heap (for example a `/MT` client and a `/MD` nvrhi.dll). A std container moved or swapped between modules would free into the wrong heap. The comments at `memory.h:94-97` and `:127` ("all instances are interchangeable") are wrong across CRTs. No code uses `STDAllocator` today. | The default `AllocatorType` becomes `IMemoryAllocator` (`DefaultMemoryAllocator` moves into `nvrhi_core`, step 5f). `is_always_equal = std::false_type`, and `operator==` compares `&m_Allocator`. With the process-wide default allocator, this is true across modules, and still correct for custom allocators. Rewrite the `memory.h:94-97` / `:127` comments for the single process-wide allocator. |
| M2 | `memory.h:23-45`, `misc.h:92` | **Debug checks are keyed on `_DEBUG`, which only MSVC defines.** donut adds `-D_DEBUG` (`donut/CMakeLists.txt:48`), but nvrhi built on its own with GCC/Clang never does. So `NVRHI_ASSERT`, `NVRHI_VERIFY` and `checked_cast` are compiled differently in different modules. That is a silent ODR violation on ELF if instantiations are merged (F4). | Introduce `NVRHI_DEBUG` (`= !defined(NDEBUG) \|\| defined(_DEBUG)`) in `memory.h`, use it everywhere instead of `_DEBUG`, and leave it overridable. F4's hidden visibility keeps the differing inline bodies private to each module. |
| T1 | `threading.h` (`SpinLock`, `Signal`, `LFStack`, `SharedSpinLock`) | **These types' layout depends on the standard library**, e.g. `std::mutex` and `std::condition_variable` (VS 2022 17.10 also changed `std::mutex`'s constructor). They must never appear in a struct that crosses a module boundary. Today they are used only inside nvrhi `src`, donut `src` and GVDB. `WeakReferenceImpl`'s `SpinLock` is only touched by the creating module once F1 is in. | Rule: `threading.h` types are module-private (alongside F3). The ABI lint fails if a public desc or interface header (`nvrhi.h`, `d3d11.h`, `d3d12.h`, `vulkan.h`, `validation.h`, `utils.h`, `common/*.h`) names them. |
| A1 | `autoptr.h:671-1010` `MonoPtr` / `DefaultDeleter` | **Frees in whichever module destroys it.** `DefaultDeleter` runs `delete` in that module, so a `MonoPtr` that crosses modules frees into the wrong heap. None of the public headers uses it today. | Rule (your decision: the original A1, with no `UserAllocated` constraint, because `std::thread` and `rtxmu::*AccelStructManager` cannot derive from it): `MonoPtr` is module-private. The ABI lint fails on `MonoPtr` in a public header. |
| D1 | `datablob.h` | **Every module gets the same class ID for its own copy.** `DataBlobImpl`, `StringDataBlobImpl`, `ProxyDataBlobImpl` and `ProxyRefDataBlobImpl` each carry an `NVRHI_CCLSID` and `NVRHI_IMPLEMENTS_CLASS`. The header is shared, so every module defines its *own* `DataBlobImpl` under the *same* class ID. A `checked_cast<DataBlobImpl*>` or a class-ID query on a blob from another module then yields a foreign layout: an F3 violation that is easy to hit by accident. No such casts exist today. | **Fixed by step 5f:** the four classes move into `nvrhi_core`'s `src`, so exactly one module owns them and their class IDs. Clients only ever see `IDataBlob`. |

**Correctness bugs found on the way (not ABI-related; small, so fixed here):**
- `SharedSpinLock::lock_shared` / `try_lock_shared` (`threading.h:294,306`) call `load(std::memory_order_release)`.
  A release order is invalid for a load: undefined behaviour, and the MSVC STL reports it in debug builds. Use
  `memory_order_relaxed`.
- `SharedSpinLock::unlock` (`:291`) uses a relaxed store; it should be `release`.
- `SharedSpinLock::is_locked_shared` (`:322-325`) tests the wrong condition: it is true when *no* writer holds the
  lock. `WaitShared` therefore backs off at the wrong time. It should wait while the exclusive bit is set.
- `LFStack` (`threading.h:183-193`) packs its counter into the pointer's upper bits only on `_WIN64` or
  Linux x86-64. On other 64-bit targets (Linux/Apple aarch64) it falls back to a 32-bit pointer mask, which
  truncates pointers.
  - Fix: use the 47/17 split for every 64-bit target with ≤48-bit user addresses, and `static_assert` on
    anything else.
- `hash_to_u32` (`misc.h:70-73`) shifts a `size_t` right by 32, which is undefined behaviour on 32-bit targets.
  Guard it with `if constexpr (sizeof(size_t) == 8)`.


### 5f. `nvrhi_core` becomes a shared library: one heap, one owner of the data blobs
`nvrhi_core` turns from a static, header-only anchor into `nvrhi_core.dll` / `libnvrhi_core.so`. It owns the
process-wide default allocator and the `IDataBlob` implementations. nvrhi, donut and every other module that uses
the COM object model link it.

**Scope (your decision: allocation only).** The object-model templates stay header-only in each module:
`ObjectImpl`, `WeakReferenceSourceImpl`, `Delegating*Impl`, the packed `WeakReferenceImpl`, the interface tables
and `MakeNewRCObj`. Only the allocator and the blobs move into the DLL.

**Export macros:** a new `include/nvrhi/core/export.h`, separate from nvrhi's `common/export.h` because it is a
different DLL.
- `NVRHI_CORE_API`: `dllexport` when `NVRHI_CORE_SHARED_LIBRARY_BUILD`, `dllimport` when
  `NVRHI_CORE_SHARED_LIBRARY_INCLUDE`, `visibility("default")` on GCC/Clang, empty when static.
- `NVRHI_CORE_C_API` = `extern "C" NVRHI_CORE_API`.
- Same rules as nvrhi: only `extern "C"`, `noexcept` exports, no exported C++ members.

**C exports** (in `src/core/`):

| Export | Replaces |
|---|---|
| `IMemoryAllocator* nvrhiCoreGetDefaultMemAllocator() noexcept` | the per-module function-local static in `memory.h:128-131` |
| `FRESULT nvrhiCoreCreateBlob(size_t size, IDataBlob** ppBlob) noexcept` | `CreateBlob` (`datablob.h:130`) |
| `FRESULT nvrhiCoreCreateStringBlob(size_t size, IDataBlob** ppBlob) noexcept` | `CreateStringBlob` (`:140`) |
| `FRESULT nvrhiCoreCreateProxyBlob(size_t size, const void* pData, IDataBlob** ppBlob) noexcept` | `CreateProxyBlob` (`:150`) |
| `FRESULT nvrhiCoreCreateProxyBlobFromSource(IDataBlob* pSource, size_t offset, size_t size, IDataBlob** ppBlob) noexcept` | `CreateProxyBlobFromSource` (`:160`) |

**Header changes:**
- **`core/memory.h`:**
  - `IMemoryAllocator` stays, with `noexcept` methods (R5).
  - `DefaultMemoryAllocator` and `details::AlignedMalloc/AlignedFree` move to `src/core/memory.cpp`.
  - `inline IMemoryAllocator* GetDefaultMemAllocator() noexcept { return nvrhiCoreGetDefaultMemAllocator(); }`
    now returns the interface type.
  - `STDAllocator` defaults to `IMemoryAllocator` (M1).
  - `NVRHI_ASSERT`/`NVRHI_VERIFY` stay inline (M2).
- **`core/foundation.h`:**
  - `MAKE_RC_OBJ` / `MAKE_RC_DELEGATING` use `MakeNewRCObj<nvrhi::IMemoryAllocator>`.
  - `UserAllocated::operator new/delete` already call `GetDefaultMemAllocator()`, so they now hit the shared heap.
  - The `friend class DefaultMemoryAllocator;` (`:514`) is dropped.
- **Effect on objects:**
  - Objects are still *destroyed* through their `ObjectWrapper` vtable in the creating module, because destructor
    code is module-specific.
  - Their *memory* now comes from, and returns to, the one core heap. So cross-module frees through
    `UserAllocated`, `nvrhi::allocator` and the default containers are all safe.
- **`core/datablob.h`:**
  - Only the inline wrappers remain, e.g. `inline FRESULT CreateBlob(size_t s, IDataBlob** pp) { return
    nvrhiCoreCreateBlob(s, pp); }`, with the same signatures, so the 14 callers don't change.
  - The four implementation classes (with their class IDs, fixing D1) move to `src/core/datablob.cpp`, where they
    may keep `std::vector`/`std::string`.
- **`src/core/core.cpp`:** keeps its `static_assert`s. `GetCoreLibraryName` (a C++ symbol) is removed, because the
  library now has real exports.

**CMake (`cmake/NvrhiCore.cmake`):**
- `option(NVRHI_CORE_BUILD_SHARED "Build nvrhi_core as a shared library" ON)` (your decision: its own option,
  default ON), then `add_library(nvrhi_core SHARED|STATIC ...)` with sources `core.cpp`, `memory.cpp` and
  `datablob.cpp`. The `nvrhi::core` alias stays, so `donut-core.cmake:42-45` is unchanged.
- **Shared:**
  - `MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"`.
  - PRIVATE `NVRHI_CORE_SHARED_LIBRARY_BUILD=1`, INTERFACE `NVRHI_CORE_SHARED_LIBRARY_INCLUDE=1`.
  - `CXX_VISIBILITY_PRESET hidden`, `VISIBILITY_INLINES_HIDDEN ON`, plus the INTERFACE visibility flags from F4.
  - `nvrhi_disable_rtti`. Installed with `RUNTIME DESTINATION bin`.
- **Static:** as today, plus the new sources. Still header-compatible, because the exports become plain C
  functions.
- **Placement:** the root `CMAKE_RUNTIME_OUTPUT_DIRECTORY` (`build/bin`) puts `nvrhi_core.dll` next to the
  samples. Check that the Asteroids benchmark's own `add_subdirectory(nvrhi)` output lands beside its executable
  (or set its runtime output directory).
- **Combination rule:** `NVRHI_BUILD_SHARED=ON` with `NVRHI_CORE_BUILD_SHARED=OFF` is a `FATAL_ERROR`. A
  static `/MT` core inside a `/MD` nvrhi.dll would mismatch CRTs and give nvrhi its own heap.

**Default ethereal build** (nvrhi static, core shared, tree `/MT`):
- donut, the samples and the static nvrhi (all `/MT`) call into `nvrhi_core.dll` (`/MD`) only through C exports
  and `IMemoryAllocator`/`IDataBlob` vtables. That is valid across CRTs.
- Each `/MT` module's own `malloc`/`new` (e.g. `MonoPtr`'s `DefaultDeleter`, std containers) still uses its own
  CRT heap. Those stay module-private (A1, T1).

### 6. Export rule: only pure C functions
**Rule:** the DLL exports only `extern "C"` functions.
- No C++ member function, constructor or destructor is exported, and none may be defined out of line in `src` if
  it is declared in a public header.
- Every non-virtual member function declared in a public header is defined `inline` in that header.
- Every virtual function is reached only through the vtable.
- `NVRHI_API` / `NVRHI_C_API` are never used inside a class scope.

**6a. Member functions move from `src/common/misc.cpp` into the headers as inline code** (no C wrapper):

| Member (today in `misc.cpp`) | Header change |
|---|---|
| `TextureSlice::resolve` (`:35`) | inline in `nvrhi.h`; `assert` becomes `NVRHI_ASSERT` |
| `TextureSubresourceSet::resolve` / `isEntireTexture` (`:58`, `:94`) | inline |
| `BufferRange::resolve` (`:113`) | inline |
| `BlendState::RenderTarget::usesConstantColor` (`:124`) | inline |
| `BlendState::usesConstantColor(uint32_t)` (`:132`) | inline. It is not exported today, so a client call would fail to link against the DLL |
| `FramebufferInfo(const FramebufferDesc&)` (`:143`) | inline constructor |
| `FramebufferInfoEx(const FramebufferDesc&)` (`:166`) | inline constructor |
| `ICommandList::setResourceStatesForFramebuffer` (`:187`) | inline non-virtual member. It only calls `setTextureState` through the vtable |

- **Write them with header-only tools:**
  - `std::max` / `std::min` become a small `nvrhi::details` helper, so `<algorithm>` stays out.
  - Where a body needs types declared later (`FramebufferDesc`, `ITexture::getDesc`, `IFramebuffer::getDesc`),
    define it out of class with `inline` after those types, still in `nvrhi.h`.
- **Delete the moved bodies from `misc.cpp`.**

**6b. Free functions become C exports with inline C++ wrappers.**
- Each one gets an `NVRHI_C_API ... noexcept` declaration in the public header, followed by its `inline` C++
  wrapper with the old name. The existing `.cpp` body is renamed to the C function.
- **Factory exports** follow R1: `FRESULT nvrhiXxxCreateDevice(const DeviceDesc*, IDevice** ppDevice) noexcept`.
  The inline C++ wrapper keeps today's signature (`DeviceHandle createDevice(const DeviceDesc&)`) by adopting
  `*ppDevice` with `TakeOver`, as decided for free functions.
- Aggregates come back through out-pointers.

| Today | C export |
|---|---|
| `verifyHeaderVersion` | `nvrhiVerifyHeaderVersion` |
| `getFormatInfo` | `const FormatInfo* nvrhiGetFormatInfo(Format)` |
| `coopvec::getDataTypeSize` / `getOptimalMatrixStride` | `nvrhiCoopVecGetDataTypeSize` / `nvrhiCoopVecGetOptimalMatrixStride` |
| `d3d11::createDevice` / `convertFormat` | `nvrhiD3D11CreateDevice(const d3d11::DeviceDesc*)` / `nvrhiD3D11ConvertFormat` |
| `d3d12::createDevice` / `convertFormat` | `nvrhiD3D12CreateDevice` / `nvrhiD3D12ConvertFormat` |
| `vulkan::createDevice` / `convertFormat` / `resultToString` | `nvrhiVulkanCreateDevice` / `nvrhiVulkanConvertFormat` / `nvrhiVulkanResultToString` |
| `validation::createValidationLayer` | `nvrhiCreateValidationLayer(IDevice*)` |
| 17 `utils::*` functions in `utils.h` | `nvrhiUtils<Name>`, e.g. `nvrhiUtilsCreateBindingSetAndLayout(..., IBindingLayout** inoutLayout, IBindingSet** outSet, bool)` |

### 7. CMake (`donut/nvrhi/CMakeLists.txt`, `cmake/NvrhiCore.cmake`)
- **Shared build:**
  - `MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"`.
  - `CXX_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN ON`.
  - `target_compile_definitions(nvrhi INTERFACE NVRHI_SHARED_LIBRARY_INCLUDE=1)` (PRIVATE `..._BUILD` already
    exists).
  - Aftermath stays allowed (step 5b).
- **CRT mismatch:** `nvrhi.dll` links the shared `nvrhi_core.dll` (step 5f requires it), so no `/MT` static core
  lands inside the `/MD` DLL.
  - Watch for the same mismatch from other static dependencies (e.g. `DirectX-Guids`). If it shows up, compile
    their sources into `nvrhi` or set their runtime to match.
- **Static build:** unchanged.

### 8. Adapt nvrhi `src` and the downstream code
- **nvrhi `src`:**
  - `.c_str()` / `.empty()` and `std::wstring(begin, end)` keep working.
  - `vulkan-shader.cpp:43` builds a `std::string` explicitly.
  - Streams use the template `operator<<`.
  - `verifyPermanentResourceState` (`state-tracking.h:139`) takes `const char*`.
  - Desc-to-desc copies are unchanged.
- **donut, ethereal-samples, nvrhi `tests/`, and the `ethereal-samples/benchmark/Asteroids` copy of nvrhi:** let
  the compiler find the remaining call sites:
  - `std::string x = desc.debugName` becomes `x(desc.debugName.c_str())`.
  - Assigning a `std::vector` to `bindings` becomes `.assign(v.begin(), v.end())`.
  - Code that relied on `nvrhi.h` pulling in `<string>` / `<vector>` / `<optional>` adds its own includes.

## Verification
1. **Header hygiene check** (new CMake script test under `NVRHI_BUILD_TESTS`, run by ctest):
   - **Why direct includes:** the allowed `<memory>` / `<functional>` pull in containers transitively on MSVC,
     so the check looks at direct includes and tokens rather than include guards.
   - **Scope:** it scans every header under `include/nvrhi/`.
   - **It fails on:**
     - Any `#include` of `<string> <vector> <optional> <array> <deque> <set> <map> <unordered_map> <list>
       <filesystem> <mutex>`.
     - `<memory>` anywhere except `core/autoptr.h`.
     - Any `std::string` / `std::vector` / `std::optional` / `std::function` / `std::shared_ptr` /
       `std::unique_ptr` / `std::weak_ptr` token.
     - `common/resource.h` including `core/foundation.h` or `core/autoptr.h`.
   - **Exceptions:** `core/foundation.h`, `core/threading.h` and `core/memory.h` are exempt from the `<mutex>`
     rule.
   - **Plus one compile-only TU per public header,** to show each one is self-contained.
   - **ABI lint** (`tests/abi_lint.py`, run by ctest when Python 3 is found; the same logic as the scan already
     done). It strips comments, tracks class scope, and fails on any of these in an `I*` interface:
     - a `virtual` whose return type *or any parameter* is a class, struct or union by value (such as an
       `AutoPtr`/`*Handle`, `TextureSubresourceSet`, or a D3D12 handle struct); references and pointers are fine
       (R1). The scanner already exists from this review: it resolves enum and scalar typedefs, and treats Vulkan
       handle types as scalars.
     - `virtual ~` (R2);
     - two virtuals with the same name (R3);
     - a virtual without `noexcept` (R5);
     - and, in any class or struct, `NVRHI_API` / `NVRHI_C_API`, or a non-virtual member function or
       constructor that is declared without a body (export rule, step 6). The only exemption is the
       declaration-only SFINAE helpers used in `decltype` (`core/autoptr.h:165,168`
       `WeakRefTypeTrait::GetWeakRef`).
     - A scan of the current headers finds exactly the step 6a members plus `utils::BitSetAllocator`, which
       moves to `src` (step 5). So once both steps are done, nothing is left over.
     - a non-interface data struct with a base class (R4);
     - `SpinLock`/`Signal`/`LFStack`/`SharedSpinLock`/`MonoPtr`/`STDAllocator` named in a public API header (T1,
       A1, M1).
   - **`core.cpp` `static_assert`s:** GUID literals with uppercase and lowercase hex produce the same value (C2),
     and all `FE_*` codes are distinct (C3).
   - **Layout asserts:** the R4 `static_assert`s (`sizeof` / `offsetof`) are compiled on every build.
   - **Cross-module `foundation.h` test** (new, `tests/core/`, Windows):
     - A test DLL built with `NVRHI_BUILD_TESTS` creates `WeakReferenceSourceImpl` and `ObjectImpl` objects with
       its own counting `IMemoryAllocator`, and hands out interface pointers.
     - The test EXE takes `AutoPtr`/`WeakPtr` references, drops the last strong reference, then the last weak
       reference, and calls `Lock()` after expiry.
     - It asserts that the DLL's allocator freed everything it allocated and the EXE's allocator freed nothing of
       it (F1). Run it with the DLL built both packed and unpacked (`NVRHI_PACK_CONTROL_BLOCK_AND_OBJECT=0/1`) and
       the EXE built with the other value, to confirm that F2 is harmless after F1.
     - It also runs a `checked_cast` within each module only (F3).
     - **Shared-heap checks (step 5f):**
       - `GetDefaultMemAllocator()` returns the same pointer in the test DLL and the EXE.
       - An `nvrhi::vector`/`nvrhi::string` filled in the DLL and destroyed in the EXE (and the reverse) frees
         cleanly with the EXE on `/MT` and the DLL on `/MD`.
       - A `CreateBlob` blob from the EXE is resized and released by the DLL.
2. **`tests/containers.cpp`:**
   - Non-trivial `T` with a constructor/destructor counter, the 23/24-char SSO boundary, `fixed_vector` overflow
     on/off (inline buffer to heap and back after a move/copy), and `static_vector`.
   - Allocator semantics per EASTL: move-assign/swap with equal vs unequal allocators, copy-ctor carries the
     source allocator, copy-assign keeps its own. Two counting `IMemoryAllocator`s check that each frees exactly
     what it allocated.
   - `sizeof` checks.
3. **Default build:** `.\build.ps1` (static, `/MT`) for Debug and Release. donut and all samples must compile.
4. **Shared build:** configure a separate dir with `-DNVRHI_BUILD_SHARED=ON` and build Debug. Do it once with
   Aftermath off and once with `DONUT_WITH_AFTERMATH=ON`, so donut's Aftermath code compiles against the new
   interfaces. Then run `aftermath_sample`.
   - `dumpbin /exports nvrhi.dll`: every name starts with `nvrhi` and none is C++-mangled (`?`). That means no
     member functions, constructors, destructors or vtables are exported.
   - **Link-level proof:** donut and all samples link against the import library with no unresolved externals.
     An out-of-line member left behind would show up here.
   - `dumpbin /dependents`: shows `ucrtbased.dll` / `VCRUNTIME140D.dll` (`/MDd`) and `nvrhi_core.dll`.
   - **`nvrhi_core.dll`:**
     - `dumpbin /exports` lists only the `nvrhiCore*` C exports.
     - In the *default* build (nvrhi static, `/MT` tree), donut and every sample start with `nvrhi_core.dll` found
       next to them in `build/bin`.
     - `-DNVRHI_BUILD_SHARED=ON -DNVRHI_CORE_BUILD_SHARED=OFF` stops configuration with the expected
       `FATAL_ERROR`.
5. **Runtime:** run DDGISample and VXGISample with `--debug` in both builds, with no new validation errors.
6. **MinGW consumer (optional, manual):** this machine has only `C:\Program Files\LLVM` (MSVC target) and no
   MinGW sysroot. If llvm-mingw or MSYS2 is installed, build a small MinGW C++ consumer against the MSVC-built
   `nvrhi.dll` / import lib. It creates a D3D12 device through `nvrhiD3D12CreateDevice` and the validation layer,
   then exercises `createTexture`/`createBuffer`, `getDesc().debugName`, `queryCoopVecFeatures`,
   `getFramebufferInfo()` and an `IMessageCallback` implemented on the MinGW side. Until that runs, MSVC ↔ MinGW
   compatibility rests on rules R1-R5, the ABI lint and the layout asserts. The cross-module `foundation.h` test
   above covers F1 (and that F2 is harmless) within the MSVC family.
