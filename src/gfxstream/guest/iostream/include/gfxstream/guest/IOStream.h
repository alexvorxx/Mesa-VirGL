/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef __IO_STREAM_H__
#define __IO_STREAM_H__

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "ErrorLog.h"

namespace gfxstream {
namespace guest {

class IOStream {
public:

    IOStream(size_t bufSize) {
        m_iostreamBuf = NULL;
        m_bufsizeOrig = bufSize;
        m_bufsize = bufSize;
        m_free = 0;
        m_refcount = 1;
    }

    void incRef() {
        __atomic_add_fetch(&m_refcount, 1, __ATOMIC_SEQ_CST);
    }

    bool decRef() {
        if (0 == __atomic_sub_fetch(&m_refcount, 1, __ATOMIC_SEQ_CST)) {
            delete this;
            return true;
        }
        return false;
    }

    virtual size_t idealAllocSize(size_t len) {
        return m_bufsize < len ? len : m_bufsize;
    }

    virtual void *allocBuffer(size_t minSize) = 0;
    virtual int commitBuffer(size_t size) = 0;
    virtual const unsigned char *readFully( void *buf, size_t len) = 0;
    virtual const unsigned char *commitBufferAndReadFully(size_t size, void *buf, size_t len) = 0;
    virtual const unsigned char *read( void *buf, size_t *inout_len) = 0;
    virtual int writeFully(const void* buf, size_t len) = 0;
    virtual int writeFullyAsync(const void* buf, size_t len) {
        return writeFully(buf, len);
    }

    virtual ~IOStream() {

        // NOTE: m_iostreamBuf is 'owned' by the child class thus we expect it to be released by it
    }

    virtual unsigned char *alloc(size_t len) {

        if (m_iostreamBuf && len > m_free) {
            if (flush() < 0) {
                ERR("Failed to flush in alloc\n");
                return NULL; // we failed to flush so something is wrong
            }
        }

        if (!m_iostreamBuf || len > m_bufsize) {
            size_t allocLen = this->idealAllocSize(len);
            m_iostreamBuf = (unsigned char *)allocBuffer(allocLen);
            if (!m_iostreamBuf) {
                ERR("Alloc (%zu bytes) failed\n", allocLen);
                return NULL;
            }
            m_bufsize = m_free = allocLen;
        }

        unsigned char *ptr;

        ptr = m_iostreamBuf + (m_bufsize - m_free);
        m_free -= len;

        return ptr;
    }

    virtual int flush() {

        if (!m_iostreamBuf || m_free == m_bufsize) return 0;

        int stat = commitBuffer(m_bufsize - m_free);
        m_iostreamBuf = NULL;
        m_free = 0;
        return stat;
    }

    const unsigned char *readback(void *buf, size_t len) {
        if (m_iostreamBuf && m_free != m_bufsize) {
            size_t size = m_bufsize - m_free;
            m_iostreamBuf = NULL;
            m_free = 0;
            return commitBufferAndReadFully(size, buf, len);
        }
        return readFully(buf, len);
    }

    // These two methods are defined and used in GLESv2_enc. Any reference
    // outside of GLESv2_enc will produce a link error. This is intentional
    // (technical debt).
    void readbackPixels(void* context, int width, int height, unsigned int format, unsigned int type, void* pixels);
    void uploadPixels(void* context, int width, int height, int depth, unsigned int format, unsigned int type, const void* pixels);


protected:
    void rewind() {
        m_iostreamBuf = NULL;
        m_bufsize = m_bufsizeOrig;
        m_free = 0;
    }

private:
    unsigned char *m_iostreamBuf;
    size_t m_bufsizeOrig;
    size_t m_bufsize;
    size_t m_free;
    uint32_t m_refcount;
};

}  // namespace guest
}  // namespace gfxstream

//
// When a client opens a connection to the renderer, it should
// send unsigned int value indicating the "clientFlags".
// The following are the bitmask of the clientFlags.
// currently only one bit is used which flags the server
// it should exit.
//
#define IOSTREAM_CLIENT_EXIT_SERVER      1

#endif
