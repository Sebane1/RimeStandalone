#pragma once
#include "pluginterfaces/base/ibstream.h"
#include <vector>
#include <atomic>

class MemoryStream : public Steinberg::IBStream {
public:
    MemoryStream(const std::vector<char>& initialData = {}) 
        : m_data(initialData), m_cursor(0), m_refCount(1) {}

    // IUnknown
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::IBStream*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refCount; }
    Steinberg::uint32 PLUGIN_API release() override {
        auto count = --m_refCount;
        if (count == 0) delete this;
        return count;
    }

    // IBStream
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead = nullptr) override {
        if (!buffer || numBytes < 0) return Steinberg::kInvalidArgument;
        
        Steinberg::int32 available = static_cast<Steinberg::int32>(m_data.size()) - m_cursor;
        Steinberg::int32 toRead = (numBytes < available) ? numBytes : available;
        
        if (toRead > 0) {
            std::memcpy(buffer, m_data.data() + m_cursor, toRead);
            m_cursor += toRead;
        }
        
        if (numBytesRead) *numBytesRead = toRead;
        // Return kResultOk if ANY data was read, because plugins often ask for large chunks
        // and expect kResultOk as long as they get data.
        return (toRead > 0 || numBytes == 0) ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten = nullptr) override {
        if (!buffer || numBytes < 0) return Steinberg::kInvalidArgument;
        
        if (m_cursor + numBytes > m_data.size()) {
            m_data.resize(m_cursor + numBytes);
        }
        
        std::memcpy(m_data.data() + m_cursor, buffer, numBytes);
        m_cursor += numBytes;
        
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result = nullptr) override {
        Steinberg::int64 newPos = m_cursor;
        switch (mode) {
            case Steinberg::IBStream::kIBSeekSet: newPos = pos; break;
            case Steinberg::IBStream::kIBSeekCur: newPos = m_cursor + pos; break;
            case Steinberg::IBStream::kIBSeekEnd: newPos = m_data.size() + pos; break;
            default: return Steinberg::kInvalidArgument;
        }
        
        if (newPos < 0 || newPos > m_data.size()) return Steinberg::kResultFalse;
        m_cursor = static_cast<Steinberg::int32>(newPos);
        
        if (result) *result = m_cursor;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = m_cursor;
        return Steinberg::kResultOk;
    }

    const std::vector<char>& getData() const { return m_data; }

private:
    std::vector<char> m_data;
    Steinberg::int32 m_cursor;
    std::atomic<Steinberg::uint32> m_refCount;
};
