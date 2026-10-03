# COM Interface and ABI Rules

These rules are **mandatory** for every API that crosses a module boundary (an EXE, DLL or shared object): nvrhi
(`nvrhi.dll`), nvrhi core (`nvrhi_core.dll`), and any module built on the nvrhi object model (donut, samples) that
publishes COM interfaces. A design that breaks one of them does not get merged. Fix the design, not the rule.

The goal: a module built with one compiler and C++ runtime (MSVC, clang-cl, MinGW GCC/Clang, GCC/Clang on Linux;
`/MT` or `/MD`; Debug or Release) works with a module built with any other.

Keywords: **must** and **must not** are absolute. **should** means "unless you can say why not in review".

## 1. What may cross a module boundary

| May cross | Must not cross |
| --- | --- |
| `extern "C"` functions (section 2) | C++ functions, member functions, constructors, destructors as symbols |
| COM interface pointers, called through their vtable (section 3) | implementation classes (`ObjectImpl`-derived), class IDs |
| data structs that follow section 4, passed by reference or pointer | `std::` types of any kind (containers, `std::string`, `std::optional`, `std::function`, smart pointers, `std::mutex`, ...) |
| `nvrhi::vector`, `nvrhi::string`, `nvrhi::fixed_vector`, `nvrhi::static_vector` | `MonoPtr`, `STDAllocator`, `SpinLock`, `Signal`, `LFStack`, `SharedSpinLock` |
| `AutoPtr<I>` / `*Handle` as data members (one pointer) | memory from a module's own `malloc`/`new` freed by another module |
| `FRESULT` error codes | C++ exceptions |

Anything in the right column is **module-private**. It may appear in a module's own sources and private headers,
never in a public header or across a boundary.

## 2. Exported symbols

1. A DLL **must** export only `extern "C"` functions. Its export table (`dumpbin /exports`) contains no C++-mangled
   name (`?...` on MSVC, `_Z...` on Itanium).
2. Every export **must** be declared with its DLL's C macro and be `noexcept`:
   - nvrhi: `NVRHI_C_API ... noexcept` (`<nvrhi/common/export.h>`), names prefixed `nvrhi`.
   - nvrhi core: `NVRHI_CORE_C_API ... noexcept` (`<nvrhi/core/export.h>`), names prefixed `nvrhiCore`.
   - A new DLL gets its own `*_API` / `*_C_API` macro pair and its own name prefix.
3. Export parameters and return values are scalars, enums or pointers only. Aggregates go in and out through
   pointers.
4. A factory export **must** return `FRESULT` and hand the object out through a pointer-to-pointer with one
   reference added: `FRESULT nvrhiD3D12CreateDevice(const d3d12::DeviceDesc* desc, d3d12::IDevice** ppDevice) noexcept;`.
5. The C++ API of a free function is an `inline` wrapper in the header that calls the export, e.g.
   `inline DeviceHandle createDevice(const DeviceDesc& d)`. No logic lives in such a wrapper beyond adapting
   arguments and adopting the result (`TakeOver`).
6. A non-virtual member function, constructor or destructor declared in a public header **must** be defined
   `inline` in that header. `NVRHI_API` / `NVRHI_C_API` never appear inside a class.
7. Shared libraries build with hidden visibility (`CXX_VISIBILITY_PRESET hidden`, `VISIBILITY_INLINES_HIDDEN`).
   Modules that implement COM objects with `<nvrhi/core/foundation.h>` inherit `-fvisibility=hidden
   -fvisibility-inlines-hidden` from `nvrhi_core` on GCC/Clang, so their template instantiations stay private.

## 3. Interfaces

### 3.1 Shape

1. An interface is a `struct` named `I` + `PascalCase`, derived from `nvrhi::IObject` (RHI objects from
   `nvrhi::IRHIObject`), with single inheritance only.
2. It **must** have a unique IID: `NVRHI_IID(IFoo, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")` before the struct, and
   `NVRHI_DECLARE_UUID_TRAITS(IFoo)` inside it. Generate a fresh GUID; never copy one.
3. It contains only pure virtual functions, inline non-virtual helpers that call them, and type aliases. It has
   no data members.
4. Lifetime is managed only by `AddRef`/`Release`. An interface **must not** declare a destructor, virtual or not
   (**R2**: MSVC gives a virtual destructor one vtable slot, Itanium ABIs two).
5. An interface **must not** have two virtual functions with the same name (**R3**: MSVC groups overloads in
   reverse declaration order). When overloads are needed, number them in declaration order: `copyTexture1`,
   `copyTexture2`, `copyTexture3`; `createGraphicsPipeline1`, `createGraphicsPipeline2`. Do not add inline wrappers
   that recreate the overloaded name.
6. Every virtual function **must** be `noexcept`, and so must every override (**R5**). The implementation catches
   what it can throw and returns an `FE_*` code (section 5).

### 3.2 Return values (**R1**, the D3D12 protocol)

A virtual function **must not** return a class, struct or union by value. MSVC passes the hidden return pointer
after `this` and returns every user-defined type through it; MinGW passes it before `this` and returns small
trivial types in `RAX`. Use one of these forms:

| The function produces | Form |
| --- | --- |
| nothing, a scalar, an enum, a `bool` | return it directly |
| an interface object | `virtual FRESULT createFoo(const FooDesc& desc, IFoo** ppFoo) noexcept = 0;`, the out-parameter last |
| a plain struct | `virtual Info& getInfo(Info& retVal, <params>) noexcept = 0;` (WIDL form with a reference: fill `retVal`, return it) |
| a native API object or handle | `NativeObject` (`void*`) |
| a non-owning view of an object the callee keeps | a raw pointer or a `const T&`, documented as "does not AddRef" |

For the interface-object form:

- On success, return `FS_OK` and store the object with **one reference added**. On failure, return an `FE_*` code
  and store `nullptr`.
- Keep the method name: `createTexture`, not `createTextureRaw`. Do not add an inline overload that returns a
  handle; callers write `nvrhi::TextureHandle tex; device->createTexture(desc, &tex);`. `&handle` releases the old
  value first.
- Former default arguments become explicit at the call site.

### 3.3 Parameters

1. No class, struct or union by value (**R1**): pass `const T&`. This also covers small trivially copyable
   structs, so the rule stays mechanical.
2. A required input or output is a C++ reference (`T&`, `const T&`).
3. An optional input or output is a pointer annotated with SAL (`<nvrhi/core/salieri.h>`, or `<sal.h>` on MSVC):
   `_In_opt_ const ComponentMapping* mapping = nullptr`, `_Out_opt_ size_t* pSize`. Never `std::optional`.
4. An interface output is `IFoo**` (section 3.2), never `IFoo*&` or a handle.
5. Strings are `const char*` (read only) or `const nvrhi::string&`. Arrays are `const T* p, uint32_t count`, or a
   `const nvrhi::vector<T>&` / `static_vector` inside a desc struct.
6. Callbacks are interfaces implemented by the caller (for example `IMessageCallback`,
   `IAftermathShaderBinaryLookup`) or a C function pointer for stateless hooks
   (`PFN_AftermathShaderHashGenerator`). Never `std::function`.

### 3.4 Evolving an interface

1. A released interface's vtable **must not** change: do not reorder, insert, remove or re-sign its virtual
   functions. To extend it, derive a new interface with a new IID (`IFoo1 : IFoo`) and have the object answer
   both IIDs in `QueryInterface`.
2. A change to any public struct layout or interface bumps `nvrhi::c_HeaderVersion`. Applications call
   `nvrhi::verifyHeaderVersion()` at startup.

## 4. Data structs that cross the boundary

1. A struct **must not** have a base class (**R4**: Itanium ABIs reuse a non-POD base's tail padding for derived
   members, MSVC does not). Repeat the fields instead, and give it a conversion helper, as `FramebufferInfoEx`
   does with `getInfo()`.
2. Enums **must** have a fixed underlying type (`enum class Format : uint8_t`).
3. Adjacent bit-fields **must** have types of the same size.
4. No `long`, `wchar_t`, `long double`, `alignas`, `#pragma pack`, `[[no_unique_address]]` or pointer-to-member.
   The layout **must not** depend on `_DEBUG`, `NDEBUG` or `NVRHI_DEBUG`.
5. Strings and arrays are `nvrhi::string`, `nvrhi::vector`, `nvrhi::fixed_vector` or `nvrhi::static_vector`
   (`<nvrhi/core/containers.h>`). Object references are `AutoPtr<I>` / `*Handle` (one pointer) or raw interface
   pointers.
6. Each struct that crosses the boundary gets `static_assert`s on its `sizeof` and on the `offsetof` of fields
   placed after padding or after another struct.
7. Member functions are `inline` in the header (section 2.6).

## 5. Error codes

Functions that can fail return `FRESULT` (`int32_t`). Test them with `NVRHI_SUCCEEDED(hr)` / `NVRHI_FAILED(hr)`.

| Code | Value | Meaning |
| --- | --- | --- |
| `FS_OK` | 0 | success |
| `FE_GENERIC_ERROR` | -1 | failure with no better code (including an unexpected exception) |
| `FE_NOINTERFACE` | -2 | `QueryInterface` for an IID the object does not implement |
| `FE_NOT_IMPLEMENT` | -3 | not implemented by this object |
| `FE_INVALID_ARGS` | -4 | invalid arguments (including validation-layer rejections) |
| `FE_NOT_ALIVE_OBJECT` | -5 | the object is being destroyed or a weak reference expired |
| `FE_NOT_FOUND` | -6 | lookup found nothing |
| `FE_WAIT_TIMEOUT` | -7 | wait timed out |
| `FE_OUT_OF_MEMORY` | -8 | allocation failed (`std::bad_alloc` caught) |
| `FE_UNSUPPORTED` | -9 | the backend or device does not support the request |

Every code **must** be distinct. New codes are negative and are appended, never renumbered.

## 6. Memory and object lifetime

1. `nvrhi_core.dll` owns the one process-wide default allocator (`nvrhi::GetDefaultMemAllocator()`).
   `MAKE_RC_OBJ`, `UserAllocated`, `nvrhi::allocator` and the nvrhi containers all use it, so memory from them may
   be freed in any module.
2. An object is always destroyed by code of the module that created it: `Release` reaches its `ObjectWrapper`
   through the vtable. Never `delete` an interface pointer.
3. Weak references go only through `IWeakReference` (`WeakPtr` holds `IWeakReference*`). Never touch another
   module's control block.
4. Class IDs (`NVRHI_CLASS_CLSID`, `NVRHI_IMPLEMENTS_CLASS`) and `checked_cast` to an implementation class are
   used only on objects created by the same module. Aggregation (`Delegating*Impl`) never spans modules.
5. A module's own `new`/`malloc`, `std::` containers, `MonoPtr` and the `threading.h` types stay inside that
   module.

## 7. Public headers

1. A public header **must not** include `<string>`, `<vector>`, `<optional>`, `<array>`, `<deque>`, `<set>`,
   `<map>`, `<unordered_map>`, `<list>`, `<filesystem>` or `<mutex>`. Only `core/autoptr.h` may include
   `<memory>`.
2. A public header compiles on its own.
3. Implementation helpers (`common/resourcebindingmap.h`, `BitSetAllocator`, debug-name generators, ...) live
   under `src/`, not `include/`.

## 8. Enforcement

These checks run under `NVRHI_BUILD_TESTS` (`ctest` in the nvrhi build directory) and **must** pass before a
merge:

| Check | Covers |
| --- | --- |
| `nvrhi_abi_lint` ([`tests/abi_lint.py`](../tests/abi_lint.py)), and `nvrhi_abi_lint_selftest` | R1 to R5, the module-private types, the export rule |
| `nvrhi_header_hygiene` ([`tests/header_hygiene.cmake`](../tests/header_hygiene.cmake)) | section 7, `noexcept` on every C export |
| `nvrhi_header_tus` | every public header compiles on its own |
| `static_assert`s in `nvrhi.h` and `src/core/core.cpp` | struct layouts, GUID parsing, distinct error codes |
| `nvrhi_test_containers`, the `cross_module` DLL/EXE tests | container semantics, freeing across `/MT` and `/MD` |

A shared build (`-DNVRHI_BUILD_SHARED=ON`) additionally checks by hand that `dumpbin /exports nvrhi.dll` and
`nvrhi_core.dll` list only `nvrhi*` / `nvrhiCore*` names.

## 9. Example

```cpp
NVRHI_IID(IWidget, "1f0c6c2e-8a51-4d7b-9a43-2b6f0f1d5e77")
struct IWidget : IRHIObject
{
    NVRHI_DECLARE_UUID_TRAITS(IWidget)

    [[nodiscard]] virtual const WidgetDesc& getDesc() const noexcept = 0;                   // non-owning view
    virtual WidgetStats& getStats(WidgetStats& retVal) noexcept = 0;                         // struct: WIDL form
    virtual FRESULT createPart(const PartDesc& desc, IPart** ppPart) noexcept = 0;           // object: FRESULT + IFoo**
    virtual void setColor1(const Color& color) noexcept = 0;                                 // numbered, not overloaded
    virtual void setColor2(_In_opt_ const Color* color, float alpha) noexcept = 0;           // optional: SAL pointer
};
typedef AutoPtr<IWidget> WidgetHandle;

NVRHI_C_API FRESULT nvrhiCreateWidget(const WidgetDesc* desc, IWidget** ppWidget) noexcept;
inline WidgetHandle createWidget(const WidgetDesc& desc)
{
    IWidget* widget = nullptr;
    nvrhiCreateWidget(&desc, &widget);
    return TakeOver(widget);
}
```

Not allowed:

```cpp
struct IWidget : IRHIObject
{
    virtual ~IWidget() = default;                                   // R2: destructor
    virtual WidgetHandle createPart(const PartDesc& desc) = 0;      // R1: class by value; R5: not noexcept
    virtual void setColor(const Color& color) = 0;                  // R3: overloaded ...
    virtual void setColor(std::optional<Color> color) = 0;          // ... and std:: type across the boundary
    std::string name;                                               // data member, std:: type
};
NVRHI_API WidgetHandle createWidget(const WidgetDesc& desc);       // exported C++ symbol
```
