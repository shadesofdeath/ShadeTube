#include "audio/MfByteStream.h"

#include "audio/ProgressiveBuffer.h"
#include "core/Utf.h"

#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>

using Microsoft::WRL::ComPtr;

namespace st::audio {

namespace {

// State object of one asynchronous read (carried inside the caller's IMFAsyncResult).
class ReadRequest final : public IUnknown {
public:
    ReadRequest(BYTE* dst, ULONG size, int64_t offset) : dst(dst), size(size), offset(offset) {}
    BYTE* const dst;
    const ULONG size;
    const int64_t offset;
    ULONG bytesRead = 0;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

private:
    ~ReadRequest() = default;
    std::atomic<ULONG> refs_{1};
};

class MfByteStream final : public IMFByteStream, public IMFAttributes, public IMFAsyncCallback {
public:
    MfByteStream(std::shared_ptr<ProgressiveBuffer> buffer, ComPtr<IMFAttributes> attrs)
        : buffer_(std::move(buffer)), attrs_(std::move(attrs)), length_(buffer_->length()) {}

    // ---- IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream))
            *ppv = static_cast<IMFByteStream*>(this);
        else if (riid == __uuidof(IMFAttributes))
            *ppv = static_cast<IMFAttributes*>(this);
        else if (riid == __uuidof(IMFAsyncCallback))
            *ppv = static_cast<IMFAsyncCallback*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // ---- IMFByteStream
    STDMETHODIMP GetCapabilities(DWORD* caps) override {
        if (!caps) return E_POINTER;
        // REMOTE + PARTIALLY_DOWNLOADED put the MPEG-4 source in progressive mode: it starts
        // delivering samples right after the header instead of scanning every fragment first
        // (measured: ~1.5 s -> ~0.3 s to the first sample on a 3.4 MB track).
        *caps = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE | MFBYTESTREAM_IS_REMOTE |
                MFBYTESTREAM_IS_PARTIALLY_DOWNLOADED;
        return S_OK;
    }
    STDMETHODIMP GetLength(QWORD* length) override {
        if (!length) return E_POINTER;
        *length = static_cast<QWORD>(length_);
        return S_OK;
    }
    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPosition(QWORD* position) override {
        if (!position) return E_POINTER;
        std::lock_guard lock(mutex_);
        *position = static_cast<QWORD>(position_);
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD position) override {
        if (position > static_cast<QWORD>(length_)) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        position_ = static_cast<int64_t>(position);
        return S_OK;
    }
    STDMETHODIMP IsEndOfStream(BOOL* eos) override {
        if (!eos) return E_POINTER;
        std::lock_guard lock(mutex_);
        *eos = position_ >= length_;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE* dst, ULONG size, ULONG* read) override {
        if (!dst || !read) return E_POINTER;
        int64_t offset;
        ULONG n;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return MF_E_SHUTDOWN;
            offset = position_;
            n = clampSize(offset, size);
            position_ += n;
        }
        *read = 0;
        const HRESULT hr = readAt(offset, dst, n);
        if (SUCCEEDED(hr)) *read = n;
        return hr;
    }
    STDMETHODIMP BeginRead(BYTE* dst, ULONG size, IMFAsyncCallback* callback, IUnknown* state) override {
        if (!dst || !callback) return E_POINTER;
        int64_t offset;
        ULONG n;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return MF_E_SHUTDOWN;
            offset = position_;
            n = clampSize(offset, size);
            position_ += n;  // the position advances when the read is issued (standard behaviour)
        }
        ComPtr<ReadRequest> request;
        request.Attach(new (std::nothrow) ReadRequest(dst, n, offset));
        if (!request) return E_OUTOFMEMORY;
        ComPtr<IMFAsyncResult> callerResult;
        HRESULT hr = MFCreateAsyncResult(request.Get(), callback, state, &callerResult);
        if (FAILED(hr)) return hr;
        // Invoke() runs on the long-function queue with `callerResult` as its state.
        return MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_LONG_FUNCTION, static_cast<IMFAsyncCallback*>(this),
                             callerResult.Get());
    }
    STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) override {
        if (!result || !read) return E_POINTER;
        *read = 0;
        ComPtr<IUnknown> object;
        HRESULT hr = result->GetObject(&object);
        if (FAILED(hr)) return hr;
        *read = static_cast<ReadRequest*>(object.Get())->bytesRead;
        return result->GetStatus();
    }
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override { return E_ACCESSDENIED; }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_ACCESSDENIED; }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override { return E_ACCESSDENIED; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* newPosition) override {
        std::lock_guard lock(mutex_);
        if (closed_) return MF_E_SHUTDOWN;
        int64_t target = origin == msoCurrent ? position_ + offset : offset;
        if (target < 0 || target > length_) return E_INVALIDARG;
        position_ = target;
        if (newPosition) *newPosition = static_cast<QWORD>(target);
        return S_OK;
    }
    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override {
        std::lock_guard lock(mutex_);
        closed_ = true;
        return S_OK;
    }

    // ---- IMFAsyncCallback (internal: executes queued reads)
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* work) override {
        ComPtr<IUnknown> state;
        if (FAILED(work->GetState(&state))) return S_OK;
        ComPtr<IMFAsyncResult> callerResult;
        if (FAILED(state.As(&callerResult))) return S_OK;
        ComPtr<IUnknown> object;
        HRESULT hr = callerResult->GetObject(&object);
        if (SUCCEEDED(hr)) {
            auto* request = static_cast<ReadRequest*>(object.Get());
            hr = readAt(request->offset, request->dst, request->size);
            if (SUCCEEDED(hr)) request->bytesRead = request->size;
        }
        callerResult->SetStatus(hr);
        MFInvokeCallback(callerResult.Get());
        return S_OK;
    }

    // ---- IMFAttributes (delegated to an internal store)
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return attrs_->Compare(o, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return attrs_->GetStringLength(k, n); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return attrs_->GetString(k, s, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return attrs_->GetAllocatedString(k, s, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attrs_->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* l) override { return attrs_->GetBlob(k, b, n, l); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return attrs_->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return attrs_->GetUnknown(k, i, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attrs_->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attrs_->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return attrs_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* dst) override { return attrs_->CopyAllItems(dst); }

private:
    ~MfByteStream() = default;

    ULONG clampSize(int64_t offset, ULONG size) const {
        return static_cast<ULONG>(std::clamp<int64_t>(length_ - offset, 0, size));
    }

    HRESULT readAt(int64_t offset, BYTE* dst, ULONG size) {
        if (size == 0) return S_OK;
        switch (buffer_->read(offset, dst, size)) {
        case ProgressiveBuffer::ReadStatus::Ok: return S_OK;
        case ProgressiveBuffer::ReadStatus::Interrupted:
        case ProgressiveBuffer::ReadStatus::Cancelled: return E_ABORT;
        case ProgressiveBuffer::ReadStatus::Failed: return MF_E_NET_READ;
        }
        return E_FAIL;
    }

    std::atomic<ULONG> refs_{1};
    const std::shared_ptr<ProgressiveBuffer> buffer_;
    const ComPtr<IMFAttributes> attrs_;
    const int64_t length_;
    std::mutex mutex_;
    int64_t position_ = 0;
    bool closed_ = false;
};

} // namespace

HRESULT createMfByteStream(std::shared_ptr<ProgressiveBuffer> buffer, const std::string& mimeType,
                           IMFByteStream** out) {
    if (!buffer || !out) return E_POINTER;
    *out = nullptr;
    if (buffer->length() <= 0) return E_UNEXPECTED;
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) return hr;
    if (!mimeType.empty()) attrs->SetString(MF_BYTESTREAM_CONTENT_TYPE, toWide(mimeType).c_str());
    auto* stream = new (std::nothrow) MfByteStream(std::move(buffer), std::move(attrs));
    if (!stream) return E_OUTOFMEMORY;
    *out = static_cast<IMFByteStream*>(stream);  // refcount 1 transferred to the caller
    return S_OK;
}

} // namespace st::audio
