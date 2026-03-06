#pragma once

// Minimal stub replacing rePlayer's IO/Stream.h
// Provides core::io::Stream interface matching what OrganyaDecoder.cpp needs.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace core { namespace io {

    class Stream {
    public:
        enum SeekWhence { kSeekBegin = 0 };

        virtual ~Stream() = default;
        virtual size_t Read(void* buffer, size_t size) = 0;
        virtual size_t Seek(int64_t offset, SeekWhence whence) = 0;
    };

}}
// namespace core::io

// A simple memory-backed stream for loading .org files from a buffer.
class MemoryStream : public core::io::Stream {
public:
    MemoryStream(const uint8_t* data, size_t size)
        : m_data(data), m_size(size), m_pos(0) {}

    size_t Read(void* buffer, size_t size) override {
        size_t avail = m_size - m_pos;
        if (size > avail) size = avail;
        memcpy(buffer, m_data + m_pos, size);
        m_pos += size;
        return size;
    }

    size_t Seek(int64_t offset, SeekWhence whence) override {
        if (whence == kSeekBegin)
            m_pos = (offset < 0) ? 0 : ((size_t)offset > m_size ? m_size : (size_t)offset);
        return m_pos;
    }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
};
