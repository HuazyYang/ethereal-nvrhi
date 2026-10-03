# 0001. Adopt a COM-like object model in NVRHI (`nvrhi::core`)

- Status: Accepted
- Date: 2026-10-02

## Context

Before this change the ethereal tree had two unrelated object models:

- **NVRHI** had its own reference counting: `IResource` (with `AddRef`, `Release`, `GetRefCount` and
  a virtual destructor), the smart pointer `RefCountPtr<T>` (a copy of WRL `ComPtr`) and the
  implementation helper `RefCounter<T>`, all in `include/nvrhi/common/resource.h`. Interfaces had no
  identifiers and no `QueryInterface`. Backends also used `std::unique_ptr` and `std::shared_ptr`.
- **donut** (fork branch `ethereal-dev`) had a COM-like model in `include/donut/core/object/`:
  `IObject` with `QueryInterface`/`AddRef`/`Release`, compile-time GUIDs (`DONUT_IID`),
  `ObjectImpl<...>` with generated interface tables, weak references (`WeakPtr`), the intrusive
  `AutoPtr<T>`, the unique-ownership `MonoPtr<T>`, `DataBlob`, and spin locks. It was header only.

donut depends on NVRHI, not the other way round. A single object model shared by both therefore has
to live in NVRHI. The donut headers were the more complete model and already had tests
(`test_auto_ptr.cpp`, `test_qi_route.cpp`).

## Decision

Move donut's object model into NVRHI as `nvrhi::core`, and make it the object model of NVRHI and of
donut.

**Placement.** The six headers `autoptr.h`, `datablob.h`, `foundation.h`, `memory.h`, `threading.h`
and `types.h` moved from `donut/include/donut/core/object/` to `include/nvrhi/core/`. They are still
header only. They arrived PascalCase (`AutoPtr.h`, `Foundation.h`, ...) and were renamed to lowercase
without separators, NVRHI's file naming (e.g. `common/resourcebindingmap.h`), in a later commit.
`memory.h` shares its name with the CRT header; that is safe because every include is path-qualified
(`<nvrhi/core/memory.h>`) and no include directory points at `include/nvrhi/core` itself.

**Library target.** `cmake/NvrhiCore.cmake` defines the static library `nvrhi_core` with the alias
`nvrhi::core`:

- It sets the public include directory and `cxx_std_17`, and links `Threads::Threads`.
- `src/core/core.cpp` is its only source. It includes every core header (a compile check),
  holds a few `static_assert`s on GUID parsing and `AutoPtr` layout, and defines
  `nvrhi::details::GetCoreLibraryName()`. That function is the library's one public symbol; without it
  MSVC warns LNK4221 (object file has no public symbols).
- `CMakeLists.txt` includes `NvrhiCore.cmake`. `nvrhi`, `nvrhi_d3d11`, `nvrhi_d3d12` and `nvrhi_vk`
  link `nvrhi_core` PUBLIC. In the `NVRHI_BUILD_SHARED=ON` build the backends are sources of `nvrhi`
  itself, which links `nvrhi_core` PUBLIC.
- `nvrhi_core` is installed and exported with `nvrhiTargets`. `src/nvrhiConfig.cmake.in` now calls
  `find_dependency(Threads)`.
- A project that needs only the object model can include `NvrhiCore.cmake` directly and link
  `nvrhi::core` (see the comment at the top of that file).

**Renames** (applied mechanically; commit `9eb1458`):

| donut | nvrhi |
| --- | --- |
| namespaces `donut`, `donut::details`, `donut::literals` | `nvrhi`, `nvrhi::details`, `nvrhi::literals` |
| `DONUT_*` macros (`DONUT_IID`, `DONUT_DECLARE_UUID_TRAITS`, `DONUT_CCLSID`, `DONUT_BEGIN_INTERFACE_TABLE`, `DONUT_ASSERT`, ...) | `NVRHI_*` |
| `donut_likely` / `donut_unlikely` | `NVRHI_LIKELY` / `NVRHI_UNLIKELY` (macros are upper case) |
| `_donut_guid` literal | `_nvrhi_guid` |
| `DonutNewOverload` | `NvrhiNewOverload` |
| `FIID_PPV_ARGS` | `NVRHI_IID_PPV_ARGS` |
| `FSUCCEEDED` / `FFAILED` | `NVRHI_SUCCEEDED` / `NVRHI_FAILED` |
| global `UUIDTraits` | global `NvrhiUUIDTraits` |
| global `__uuid_of<T>()` | `nvrhi::uuid_of<T>()` |
| `<donut/core/object/X.h>`, guards `DONUT_CORE_OBJECT_*_H` | `<nvrhi/core/X.h>`, guards `NVRHI_CORE_*_H` |

**`NvrhiUUIDTraits` stays in the global namespace.** `NVRHI_IID`, `NVRHI_CCLSID` and `NVRHI_SCLSID`
expand to an explicit specialization `template <> struct ::NvrhiUUIDTraits<struct X>` at the point
where the interface is declared, which may be any namespace (`nvrhi`, `nvrhi::d3d12`, a test
namespace, donut code). Specializing a template from outside its enclosing namespace relies on CWG 727.
MSVC accepts it for a global template only and rejects it for a template in a named namespace (C2888).
So the trait cannot move into `nvrhi::details`; it is only renamed with an `Nvrhi` prefix to avoid
collisions. The comment above its declaration in `types.h` records this.

**The lookup function is `nvrhi::uuid_of`.** Only the function that callers use moved into the
namespace. `__uuid_of` was a reserved identifier (double underscore) and is close to MSVC's
`__uuidof`. `nvrhi::uuid_of<T>()` forwards to `::NvrhiUUIDTraits<T>::uuid_of()` (or, under Clang, to
`T::this_uuid()`; see below).

**Clang branch.** Clang-based tools do not implement CWG 727, so under `__clang__` `NvrhiUUIDTraits` is
a plain struct whose `uuid_of<T>()` returns `T::this_uuid()`, and `NVRHI_DECLARE_UUID_TRAITS` defines
that member. This branch existed in donut and was kept.

**`GetDefaultMemAllocator()` stays inline.** The approved plan put it out of line in `core.cpp`. It was
kept inline in `memory.h` instead: `DefaultMemoryAllocator` is stateless and has a `constexpr`
constructor, so the function-local static is constant-initialized (no guard), and one instance per
module on Windows (one per process on ELF) is harmless. The comment above the function says so.
Keeping it inline also keeps the headers usable without linking `nvrhi_core`.

**donut with `DONUT_WITH_NVRHI=OFF`.** `donut_core` must still build without NVRHI. In
`donut-core.cmake`:

```cmake
if (NOT TARGET nvrhi::core)
    include(${CMAKE_CURRENT_SOURCE_DIR}/nvrhi/cmake/NvrhiCore.cmake)
endif()
target_link_libraries(donut_core nvrhi::core)
```

With NVRHI on, `add_subdirectory(nvrhi)` runs before `donut-core.cmake` and defines the target; with
NVRHI off, only the core library is added from the submodule.

**Base classes** (updated for nvrhi `e8a7951`). The four base classes of
reference-counted objects are flat variadic templates in `foundation.h`: `ObjectImpl<Bases...>`,
`WeakReferenceSourceImpl<Bases...>`, `DelegatingObjectImpl<Bases...>` and
`DelegatingWeakReferenceSourceImpl<Bases...>`. Each derives directly from `Bases...` and owns the reference
count (`ObjectImpl`), the packed control block (`WeakReferenceSourceImpl`) or the owner pointer (the delegating
ones, which forward `AddRef`, `Release`, `QueryInterface` and `GetWeakReference` to the owner). Bases are
interfaces and mixins only; an implementation class is not a valid base (`details::IsObjectImpl`, a
`static_assert` in each template). A class that builds on an implementation derives from it directly
(`class Bar : public Foo`). The donut code had a "root layer" / "pass-through layer" split (`QIRootLayer`,
`QIPassThroughLayer`, `QILayer`, `QITraits<QILifetime>`) that let `ObjectImpl<Foo>` reuse `Foo`'s count; it
was removed, see [ADR 0007](0007-explicit-queryinterface-tables.md), "Flat base classes".

**Hygiene fixes made while moving** (all in `9eb1458`; found by compiling the headers as a library at
/W4 and by the moved tests):

- Missing std includes (`<cstddef>`, `<type_traits>` in `types.h`; `<cstddef>`, `<functional>`,
  `<memory>`, `<utility>` in `autoptr.h`; `<cstring>` in `foundation.h`). `autoptr.h` now includes `memory.h`,
  because it uses `NVRHI_ASSERT`.
- The duplicate likely/unlikely block in `foundation.h` was removed (`memory.h` defines them).
- `RouteMemberQueryInterface` called `member.QIT_::NonDelegatingQueryInterface`; the template parameter
  is `QIT`.
- `WeakReferenceImpl::IsExpired()` returned the inverse of its name.
- `std::hash<WeakPtr<T>>` constructed its hasher with an argument; it now hashes `UnsafeRawPtr()` with
  `std::hash<const T*>{}`.
- `MonoPtr<T[]>`: the default and `nullptr_t` constructors lacked `Dx2 = Dx`, and the move constructor
  lacked the `OneThenVariadicArgs` tag.
- The `QITraits` member alias in the four root layers and the pass-through layer named itself
  (`using QITraits = QITraits<...>`); in the pass-through layer that found the base's alias. They now
  name `nvrhi::details::QITraits`. (The layers and `QITraits` were removed later; see "Base classes"
  above.)
- The const `CompressedPair::GetFirst()` returned a non-const reference.
- `ObjectWrapperStorage` sized its buffer with `ObjectWrapper<IObject, IMemoryAllocator>`, which
  instantiated a `DestroyObject` deleting through `IObject` (no virtual destructor). It now uses a
  layout-equivalent stub.
- /W4 warnings: unused parameters, `strncpy` in `StringDataBlobImpl`, and
  `fetch_add(-1)` on an unsigned counter in the shared spin lock, now `fetch_sub(1)`.

**Tests.** `test_auto_ptr.cpp` and `test_qi_route.cpp` moved from `donut/tests/src/core/` to
`tests/core/`. They build as `nvrhi_test_auto_ptr` and `nvrhi_test_qi_route` when
`NVRHI_BUILD_TESTS=ON` and `find_package(GTest CONFIG QUIET)` succeeds (pass `-DGTest_DIR=...`).
Visual Leak Detector is optional: it is linked into `nvrhi_test_auto_ptr` only when the environment
variable `VLD_INSTALL_DIR` is set.

## Consequences

### Positive

- One object model for NVRHI, donut and the samples, with one set of tests, owned by the lowest layer.
- NVRHI gains `QueryInterface`, IIDs, weak references and allocator-aware creation, which ADRs
  0002-0005 build on.
- The object model can be used without the rest of NVRHI (`NvrhiCore.cmake` alone).
- Several latent bugs in the donut copy were fixed (see the list above).

### Negative

- Every donut and sample source that used the object model had to change: includes, the `donut::`
  qualifier and the `DONUT_*` macros (donut `8ec223c`, about 90 files; etherealsamples `925c412`).
- NVRHI now carries about 3,400 lines of template-heavy headers that upstream NVRHI does not have.
  Merges from upstream must keep `include/nvrhi/core` and the CMake hooks.
- The name `NvrhiUUIDTraits` remains a global symbol.

### Risks

- The specialization trick depends on CWG 727 behavior. A compiler that rejects specializing a
  global template from a named namespace would need the Clang branch.
- `GetDefaultMemAllocator()` returns a different instance per DLL on Windows. This is safe only while
  `DefaultMemoryAllocator` stays stateless. An allocator with state would need an out-of-line,
  exported instance.
- `donut/tools/Gen-Interface.ps1` still emits `DONUT_IID` / `DONUT_CCLSID` snippets. It was missed by
  the C++ rename (see the plan's follow-ups).

## Alternatives considered

Forwarding shims were ruled out explicitly when the plan was approved. The last two items are points
where the implementation departed from the plan.

- **Forwarding shims in donut** (keep `donut/core/object/*.h` as headers that include the NVRHI ones
  and alias `donut::AutoPtr = nvrhi::AutoPtr`, `#define DONUT_IID NVRHI_IID`, ...). Rejected: two
  spellings of every name would stay in use indefinitely, macros cannot be aliased cleanly across
  namespaces (the IID specializations must name `::NvrhiUUIDTraits`), and donut is a fork under the
  same control, so updating it directly was cheap.
- **Keep the model in donut and have NVRHI depend on donut.** Rejected: it inverts the dependency
  between the two projects.
- **Keep both models.** Rejected: NVRHI objects could not take part in donut's `QueryInterface` and
  weak references, and every boundary would need adapters.
- **Move `NvrhiUUIDTraits` into `nvrhi::details`** (as the plan first proposed). Rejected: C2888 on
  MSVC, as explained above.
- **Out-of-line `GetDefaultMemAllocator()` in `core.cpp`** (the plan). Rejected: no benefit for a
  stateless allocator, and it would force every user of the headers to link `nvrhi_core`.

## References

- nvrhi `lithereal-dev`: `9eb1458` core: add nvrhi::core object model (moved from donut core/object).
- donut `ethereal-dev`: `8ec223c` core: use nvrhi::core object model; remove donut/core/object.
- etherealsamples `main`: `925c412` Use nvrhi::core object model (moved from donut core/object).
- aggregate root `main`: `347d6b0` bump donut (nvrhi::core).
- Files: `cmake/NvrhiCore.cmake`, `include/nvrhi/core/*.h`, `src/core/core.cpp`,
  `src/nvrhiConfig.cmake.in`, `tests/CMakeLists.txt`, `tests/core/test_auto_ptr.cpp`,
  `tests/core/test_qi_route.cpp`; donut `donut-core.cmake`, `tests/test-core.cmake`.
- Plan: [`../plans/2026-10-02-object-model-refactor.md`](../plans/2026-10-02-object-model-refactor.md),
  step 2.
