/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// The IDataBlob implementations behind nvrhiCoreCreate*Blob. The classes and their class IDs belong to this
// module only: other modules see IDataBlob, never these classes (the module-private rule, foundation.h).

#include <nvrhi/core/datablob.h>
#include <nvrhi/core/foundation.h>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace nvrhi {
namespace details {

NVRHI_CLASS_CLSID(DataBlobImpl, "405202ca-4daa-459c-9da8-6996ca3fb1d4")
class DataBlobImpl final : public ObjectImpl<IDataBlob> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(DataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_IMPLEMENTS_CLASS(DataBlobImpl)
    NVRHI_END_INTERFACE_TABLE()

    explicit DataBlobImpl(size_t InitialSize) : m_DataBuff(InitialSize) {}

    /// Sets the size of the internal data buffer
    void Resize(size_t NewSize) noexcept override { m_DataBuff.resize(NewSize); }

    /// Returns the size of the internal data buffer
    size_t GetSize() noexcept override { return m_DataBuff.size(); }

    /// Returns the pointer to the internal data buffer
    void* GetDataPtr() noexcept override { return m_DataBuff.data(); }

 private:
    std::vector<uint8_t> m_DataBuff;
};

/// String data blob implementation.
NVRHI_CLASS_CLSID(StringDataBlobImpl, "2bf21355-9bf0-4ed4-b2e9-e5a45a25cfa2")
class StringDataBlobImpl final : public ObjectImpl<IDataBlob> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(StringDataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StringDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_IMPLEMENTS_CLASS(StringDataBlobImpl)
    NVRHI_END_INTERFACE_TABLE()

    explicit StringDataBlobImpl(size_t Size) { m_String.resize(Size); }

    /// Sets the size of the internal data buffer
    void Resize(size_t NewSize) noexcept override { m_String.resize(NewSize); }

    /// Returns the size of the internal data buffer
    size_t GetSize() noexcept override { return m_String.length(); }

    /// Returns the pointer to the internal data buffer
    void* GetDataPtr() noexcept override { return &m_String[0]; }

 private:
    std::string m_String;
};

NVRHI_CLASS_CLSID(ProxyDataBlobImpl, "d1373bc6-c59a-40c5-ac46-56d299206d43")
class ProxyDataBlobImpl final : public ObjectImpl<IDataBlob> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(ProxyDataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ProxyDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_IMPLEMENTS_CLASS(ProxyDataBlobImpl)
    NVRHI_END_INTERFACE_TABLE()

    ProxyDataBlobImpl(size_t Size, const void* pData) : m_pData{pData}, m_Size{Size} {}

    void Resize(size_t /*NewSize*/) noexcept override { NVRHI_VERIFY(false, "Operation forbidden"); }

    size_t GetSize() noexcept override { return m_Size; }

    void* GetDataPtr() noexcept override { return const_cast<void*>(m_pData); }

 private:
    const void* const m_pData;
    const size_t m_Size;
};

NVRHI_CLASS_CLSID(ProxyRefDataBlobImpl, "26307b63-679b-4182-b086-37c7c6078e16")
class ProxyRefDataBlobImpl final : public ObjectImpl<IDataBlob> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(ProxyRefDataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ProxyRefDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_IMPLEMENTS_CLASS(ProxyRefDataBlobImpl)
    NVRHI_END_INTERFACE_TABLE()

    ProxyRefDataBlobImpl(IDataBlob* pSource, size_t Offset, size_t Size)
        : m_pSource(pSource), m_Offset(Offset), m_Size(Size) {
        SafeAddRef(m_pSource);
        if (!m_pSource) m_Size = m_Offset = 0;
    }

    ~ProxyRefDataBlobImpl() { SafeRelease(m_pSource); }

    void Resize(size_t /*NewSize*/) noexcept override { NVRHI_VERIFY(false, "Operation forbidden"); }

    size_t GetSize() noexcept override { return m_Size; }

    void* GetDataPtr() noexcept override {
        return m_pSource ? static_cast<uint8_t*>(m_pSource->GetDataPtr()) + m_Offset : nullptr;
    }

 private:
    IDataBlob* m_pSource;
    size_t m_Offset;
    size_t m_Size;
};

// MAKE_RC_OBJ returns the object with its one reference, which goes to *ppBlob. Nothing escapes the C
// boundary: a constructor that runs out of memory (std::bad_alloc from std::vector / std::string) or an
// allocator that returns null becomes FE_OUT_OF_MEMORY.
template <typename Blob, typename... Args>
static FRESULT CreateBlobObject(IDataBlob** ppBlob, Args&&... args) noexcept {
    if (!ppBlob) return FE_INVALID_ARGS;
    *ppBlob = nullptr;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        *ppBlob = MAKE_RC_OBJ(Blob, std::forward<Args>(args)...);
    } catch (const std::bad_alloc&) {
        return FE_OUT_OF_MEMORY;
    } catch (...) {
        return FE_GENERIC_ERROR;
    }
#else
    *ppBlob = MAKE_RC_OBJ(Blob, std::forward<Args>(args)...);
#endif
    return *ppBlob ? FS_OK : FE_OUT_OF_MEMORY;
}

}  // namespace details

NVRHI_CORE_C_API FRESULT nvrhiCoreCreateBlob(size_t Size, IDataBlob** ppBlob) noexcept {
    return details::CreateBlobObject<details::DataBlobImpl>(ppBlob, Size);
}

NVRHI_CORE_C_API FRESULT nvrhiCoreCreateStringBlob(size_t Size, IDataBlob** ppBlob) noexcept {
    return details::CreateBlobObject<details::StringDataBlobImpl>(ppBlob, Size);
}

NVRHI_CORE_C_API FRESULT nvrhiCoreCreateProxyBlob(size_t Size, const void* pData, IDataBlob** ppBlob) noexcept {
    return details::CreateBlobObject<details::ProxyDataBlobImpl>(ppBlob, Size, pData);
}

NVRHI_CORE_C_API FRESULT nvrhiCoreCreateProxyBlobFromSource(IDataBlob* pSource, size_t Offset, size_t Size,
                                                            IDataBlob** ppBlob) noexcept {
    return details::CreateBlobObject<details::ProxyRefDataBlobImpl>(ppBlob, pSource, Offset, Size);
}

}  // namespace nvrhi
