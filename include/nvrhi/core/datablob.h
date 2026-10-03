#ifndef NVRHI_CORE_DATABLOB_H
#define NVRHI_CORE_DATABLOB_H
#include <nvrhi/core/export.h>
#include <nvrhi/core/types.h>
#include <cstddef>

// IDataBlob implementations. They live in nvrhi_core (src/core/datablob.cpp): one module owns the classes and
// their class IDs, and every other module sees the blobs through IDataBlob only.

namespace nvrhi {

// The C exports of nvrhi_core. Each one returns FS_OK and a new blob with one reference in *ppBlob, or an
// FE_* code (FE_INVALID_ARGS for a null ppBlob, FE_OUT_OF_MEMORY) with *ppBlob set to null.

/// A blob that owns Size bytes (zero-initialized), resizable.
NVRHI_CORE_C_API FRESULT nvrhiCoreCreateBlob(size_t Size, IDataBlob** ppBlob) noexcept;

/// A blob that owns a string of Size characters (zero-initialized, plus a terminator), resizable.
NVRHI_CORE_C_API FRESULT nvrhiCoreCreateStringBlob(size_t Size, IDataBlob** ppBlob) noexcept;

/// A blob over Size bytes at pData, which the caller keeps alive; not resizable.
NVRHI_CORE_C_API FRESULT nvrhiCoreCreateProxyBlob(size_t Size, const void* pData, IDataBlob** ppBlob) noexcept;

/// A blob over Size bytes at Offset in pSource, which it keeps a reference to; not resizable.
NVRHI_CORE_C_API FRESULT nvrhiCoreCreateProxyBlobFromSource(IDataBlob* pSource, size_t Offset, size_t Size,
                                                            IDataBlob** ppBlob) noexcept;

inline FRESULT CreateBlob(size_t Size, IDataBlob** ppBlob) { return nvrhiCoreCreateBlob(Size, ppBlob); }

inline FRESULT CreateStringBlob(size_t Size, IDataBlob** ppBlob) { return nvrhiCoreCreateStringBlob(Size, ppBlob); }

inline FRESULT CreateProxyBlob(size_t Size, const void* pData, IDataBlob** ppBlob) {
    return nvrhiCoreCreateProxyBlob(Size, pData, ppBlob);
}

inline FRESULT CreateProxyBlobFromSource(IDataBlob* pSource, size_t Offset, size_t Size, IDataBlob** ppBlob) {
    return nvrhiCoreCreateProxyBlobFromSource(pSource, Offset, Size, ppBlob);
}

}  // namespace nvrhi

#endif /* NVRHI_CORE_DATABLOB_H */
