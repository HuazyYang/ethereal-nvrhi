#ifndef NVRHI_COMMON_EXPORT_H
#define NVRHI_COMMON_EXPORT_H

// Export macros of nvrhi (separate from nvrhi_core's, include/nvrhi/core/export.h: a different module).
//
// - NVRHI_SHARED_LIBRARY_BUILD: compiling nvrhi as a shared library (PRIVATE definition).
// - NVRHI_SHARED_LIBRARY_INCLUDE: compiling a module that links the shared nvrhi (INTERFACE definition).
// - neither: nvrhi is a static library.
//
// The library exports only extern "C" functions (NVRHI_C_API), each one noexcept, taking and returning scalars,
// enums and pointers only; aggregates go through out-pointers. No C++ member function, constructor or destructor
// is exported: non-virtual members are inline in the public headers, virtual ones are reached through vtables.
// NVRHI_API and NVRHI_C_API are never used inside a class scope.
#if defined(NVRHI_SHARED_LIBRARY_BUILD)
#if defined(_WIN32)
#define NVRHI_API __declspec(dllexport)
#else
#define NVRHI_API __attribute__((visibility("default")))
#endif
#elif defined(NVRHI_SHARED_LIBRARY_INCLUDE)
#if defined(_WIN32)
#define NVRHI_API __declspec(dllimport)
#else
#define NVRHI_API __attribute__((visibility("default")))
#endif
#else
#define NVRHI_API
#endif

#define NVRHI_EXTERN_C extern "C"
#define NVRHI_C_API NVRHI_EXTERN_C NVRHI_API

#endif /* NVRHI_COMMON_EXPORT_H */
