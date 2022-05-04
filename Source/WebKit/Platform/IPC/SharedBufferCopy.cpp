/*
 * Copyright (C) 2018-2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SharedBufferCopy.h"

#include "Decoder.h"
#include "Encoder.h"
#include "WebCoreArgumentCoders.h"
#include <WebCore/ProcessIdentity.h>

namespace IPC {

using namespace WebCore;
using namespace WebKit;

void SharedBufferCopy::encode(Encoder& encoder) const
{
#if USE(UNIX_DOMAIN_SOCKETS)
    encoder << m_buffer;
#else
    bool isNull = !m_buffer;
    encoder << isNull;
    if (isNull)
        return;
    uint64_t bufferSize = m_buffer->size();
    encoder << bufferSize;
    if (!bufferSize)
        return;

    SharedMemory::Handle handle;
    {
        auto sharedMemoryBuffer = SharedMemory::copyBuffer(*m_buffer);
        sharedMemoryBuffer->createHandle(handle, SharedMemory::Protection::ReadOnly);
    }
    encoder << SharedMemory::IPCHandle { WTFMove(handle), bufferSize };
#endif
}

std::optional<SharedBufferCopy> SharedBufferCopy::decode(Decoder& decoder)
{
    RefPtr<SharedBuffer> buffer;
#if USE(UNIX_DOMAIN_SOCKETS)
    if (!decoder.decode(buffer))
        return std::nullopt;
    return { IPC::SharedBufferCopy(WTFMove(buffer)) };
#else
    std::optional<bool> isNull;
    decoder >> isNull;
    if (!isNull)
        return std::nullopt;

    if (*isNull)
        return { IPC::SharedBufferCopy(WTFMove(buffer)) };

    uint64_t bufferSize = 0;
    if (!decoder.decode(bufferSize))
        return std::nullopt;

    if (!bufferSize) {
        buffer = SharedBuffer::create();
        return { IPC::SharedBufferCopy(WTFMove(buffer)) };
    }

    SharedMemory::IPCHandle ipcHandle;
    if (!decoder.decode(ipcHandle))
        return std::nullopt;

    auto sharedMemoryBuffer = SharedMemory::map(ipcHandle.handle, SharedMemory::Protection::ReadOnly);
    if (!sharedMemoryBuffer)
        return std::nullopt;

    if (sharedMemoryBuffer->size() < bufferSize)
        return std::nullopt;

    return { IPC::SharedBufferCopy(sharedMemoryBuffer.releaseNonNull(), bufferSize) };
#endif
}

RefPtr<WebCore::SharedBuffer> SharedBufferCopy::unsafeBuffer() const
{
    RELEASE_ASSERT_WITH_MESSAGE(isEmpty() || (!m_buffer && m_memory), "Must only be called on IPC's receiver side");

    if (isNull())
        return nullptr;
    if (m_buffer)
        return downcast<WebCore::SharedBuffer>(m_buffer.get());

    return m_memory->createSharedBuffer(m_size);
}

const uint8_t* SharedBufferCopy::data() const
{
    RELEASE_ASSERT_WITH_MESSAGE(isEmpty() || (!m_buffer && m_memory), "Must only be called on IPC's receiver side");

    if (isNull())
        return nullptr;
    if (m_buffer)
        return downcast<SharedBuffer>(m_buffer.get())->data();

    return static_cast<uint8_t*>(m_memory->data());
}

RefPtr<WebCore::SharedBuffer> SharedBufferCopy::bufferWithOwner(const WebCore::ProcessIdentity& identity, WebKit::MemoryLedger memoryLedger) const
{
    if (isNull())
        return nullptr;
    if (m_buffer)
        return m_buffer->makeContiguous();

    SharedMemory::Handle handle;
    auto sharedMemory = SharedMemory::allocate(m_size);
    if (!sharedMemory)
        return nullptr;
    memcpy(sharedMemory->data(), m_memory->data(), m_size);
    sharedMemory->createHandle(handle, SharedMemory::Protection::ReadOnly);
    handle.transferOwnershipOfMemory(identity, memoryLedger);

    return sharedMemory->createSharedBuffer(m_size);
}

} // namespace IPC
