#ifndef NVRHI_CORE_EXPORT_H
#define NVRHI_CORE_EXPORT_H

// Export macros of nvrhi_core (cmake/NvrhiCore.cmake), a library of its own: separate from nvrhi's export
// macros, because nvrhi_core.dll and nvrhi.dll are different modules.
//
// - NVRHI_CORE_SHARED_LIBRARY_BUILD: compiling nvrhi_core as a shared library (PRIVATE definition).
// - NVRHI_CORE_SHARED_LIBRARY_INCLUDE: compiling a module that links the shared nvrhi_core (INTERFACE
//   definition).
// - neither: nvrhi_core is a static library.
//
// The library exports only extern "C" functions (NVRHI_CORE_C_API), each one noexcept, taking and returning
// scalars, enums and pointers only. No C++ member function, constructor or destructor is exported: the object
// model stays header only, and other modules reach nvrhi_core's objects through their interface vtables.
#if defined(NVRHI_CORE_SHARED_LIBRARY_BUILD)
#if defined(_WIN32)
#define NVRHI_CORE_API __declspec(dllexport)
#else
#define NVRHI_CORE_API __attribute__((visibility("default")))
#endif
#elif defined(NVRHI_CORE_SHARED_LIBRARY_INCLUDE)
#if defined(_WIN32)
#define NVRHI_CORE_API __declspec(dllimport)
#else
#define NVRHI_CORE_API __attribute__((visibility("default")))
#endif
#else
#define NVRHI_CORE_API
#endif

#define NVRHI_CORE_C_API extern "C" NVRHI_CORE_API

#endif /* NVRHI_CORE_EXPORT_H */
