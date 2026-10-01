#ifndef NVRHI_CORE_DATABLOB_H
#define NVRHI_CORE_DATABLOB_H
#include <nvrhi/core/Foundation.h>
#include <cstring>
#include <string>
#include <vector>

// IDataBlob implementations, header only like the rest of Foundation.

namespace nvrhi {

/// Base interface for a data blob
NVRHI_CCLSID(DataBlobImpl, "405202ca-4daa-459c-9da8-6996ca3fb1d4")
class DataBlobImpl final : public ObjectImpl<IDataBlob> {
 public:
    NVRHI_DECLARE_UUID_TRAITS(DataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(DataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(DataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_END_INTERFACE_TABLE()

    DataBlobImpl(size_t InitialSize, const void *pData = nullptr)
        : m_DataBuff(InitialSize) {
        if (!m_DataBuff.empty() && pData != nullptr) {
            std::memcpy(m_DataBuff.data(), pData, InitialSize);
        }
    }

    /// Sets the size of the internal data buffer
    void Resize(size_t NewSize) override { m_DataBuff.resize(NewSize); }

    /// Returns the size of the internal data buffer
    size_t GetSize() override { return m_DataBuff.size(); }

    /// Returns the pointer to the internal data buffer
    void *GetDataPtr() override { return m_DataBuff.data(); }

 private:
    std::vector<uint8_t> m_DataBuff;
};

/// String data blob implementation.
NVRHI_CCLSID(StringDataBlobImpl, "2bf21355-9bf0-4ed4-b2e9-e5a45a25cfa2")
class StringDataBlobImpl : public ObjectImpl<IDataBlob> {
    NVRHI_DECLARE_UUID_TRAITS(StringDataBlobImpl)
 public:
    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(StringDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(StringDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_END_INTERFACE_TABLE()

    /// Sets the size of the internal data buffer
    virtual void Resize(size_t NewSize) override { m_String.resize(NewSize); }

    /// Returns the size of the internal data buffer
    virtual size_t GetSize() override { return m_String.length(); }

    /// Returns the pointer to the internal data buffer
    virtual void *GetDataPtr() override { return &m_String[0]; }

    StringDataBlobImpl(size_t Size, const char *pData = nullptr) {
        m_String.resize(Size);
        // Like strncpy: up to Size characters, stopping at the terminator (the rest stays zero).
        for (size_t i = 0; pData && i < Size && pData[i] != 0; ++i) m_String[i] = pData[i];
    }

 private:
    std::string m_String;
};

NVRHI_CCLSID(ProxyDataBlobImpl, "d1373bc6-c59a-40c5-ac46-56d299206d43")
class ProxyDataBlobImpl : public ObjectImpl<IDataBlob> {
public:
    NVRHI_DECLARE_UUID_TRAITS(ProxyDataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ProxyDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(ProxyDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_END_INTERFACE_TABLE()

    virtual void Resize(size_t /*NewSize*/) override {
        NVRHI_VERIFY(false, "Operation forbidden");
    }

    virtual size_t GetSize() override { return m_Size; }

    virtual void *GetDataPtr() override { return const_cast<void *>(m_pData); }

    ProxyDataBlobImpl(size_t Size, const void *pData) : m_pData{pData}, m_Size{Size} {}

 private:
    const void *const m_pData;
    const size_t m_Size;
};

NVRHI_CCLSID(ProxyRefDataBlobImpl, "26307b63-679b-4182-b086-37c7c6078e16")
class ProxyRefDataBlobImpl : public ObjectImpl<IDataBlob> {
public:
    NVRHI_DECLARE_UUID_TRAITS(ProxyRefDataBlobImpl)

    NVRHI_BEGIN_INTERFACE_TABLE_INLINE(ProxyRefDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(ProxyRefDataBlobImpl)
    NVRHI_IMPLEMENTS_INTERFACE(IDataBlob)
    NVRHI_END_INTERFACE_TABLE()

    void Resize(size_t /*NewSize*/) override {
        NVRHI_VERIFY(false, "Operation forbidden");
    }

    size_t GetSize() override { return m_Size; }

    void *GetDataPtr() override { return  m_pSource ? (uint8_t *)m_pSource->GetDataPtr() + m_Offset : 0; }

    ProxyRefDataBlobImpl(IDataBlob *pSource, size_t Offset, size_t Size)
        : m_pSource(pSource), m_Offset(Offset), m_Size(Size) {
        SafeAddRef(m_pSource);
        if(!m_pSource)
            m_Size = m_Offset = 0;
    }

    ~ProxyRefDataBlobImpl() { SafeRelease(m_pSource); }

 private:
    IDataBlob *m_pSource;
    size_t m_Offset;
    size_t m_Size;
};

inline FRESULT CreateBlob(size_t Size, IDataBlob **ppBlob) {
    auto blob = MAKE_RC_OBJ(DataBlobImpl, Size);
    if (ppBlob) {
        *ppBlob = blob;
        blob->AddRef();
    }
    blob->Release();
    return FS_OK;
}

inline FRESULT CreateStringBlob(size_t Size, IDataBlob **ppBlob) {
    auto blob = MAKE_RC_OBJ(StringDataBlobImpl, Size);
    if (ppBlob) {
        *ppBlob = blob;
        blob->AddRef();
    }
    blob->Release();
    return FS_OK;
}

inline FRESULT CreateProxyBlob(size_t Size, const void *pData, IDataBlob **ppBlob) {
    auto blob = MAKE_RC_OBJ(ProxyDataBlobImpl, Size, pData);
    if (ppBlob) {
        *ppBlob = blob;
        blob->AddRef();
    }
    blob->Release();
    return FS_OK;
}

inline FRESULT CreateProxyBlobFromSource(IDataBlob *pSource, size_t Offset, size_t Size,
                                  IDataBlob **ppBlob) {
    auto blob = MAKE_RC_OBJ(ProxyRefDataBlobImpl, pSource, Offset, Size);
    if(ppBlob) {
        *ppBlob = blob;
        blob->AddRef();
    }
    blob->Release();
    return FS_OK;
}

}  // namespace nvrhi

#endif /* NVRHI_CORE_DATABLOB_H */
