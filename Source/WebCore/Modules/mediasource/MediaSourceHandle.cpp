/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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
#include "MediaSourceHandle.h"

#if ENABLE(MEDIA_SOURCE_IN_WORKERS)

#include "MediaSourcePrivate.h"
#include "MediaSourcePrivateClient.h"

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(MediaSourceHandle);

class MediaSourceHandle::Dispatcher final : public ThreadSafeRefCounted<Dispatcher> {
public:
    static Ref<Dispatcher> create(Function<void(Function<void()>&&)>&& dispatcher) { return adoptRef(*new Dispatcher(WTFMove(dispatcher))); }

    void dispatch(Function<void()>&& task) const
    {
        m_dispatcher(WTFMove(task));
    }
private:
    Dispatcher(Function<void(Function<void()>&&)>&& dispatcher)
        : m_dispatcher(WTFMove(dispatcher))
    {
    }
    const Function<void(Function<void()>&&)> m_dispatcher;
};

Ref<MediaSourceHandle> MediaSourceHandle::create(MediaSourcePrivateClient& client, Function<void(Function<void()>&&)>&& dispatcher)
{
    return adoptRef(*new MediaSourceHandle(client, WTFMove(dispatcher)));
}

Ref<MediaSourceHandle> MediaSourceHandle::create(Ref<MediaSourceHandle>&& other)
{
    other->setDetached(false);
    return other;
}

MediaSourceHandle::MediaSourceHandle(MediaSourcePrivateClient& client, Function<void(Function<void()>&&)>&& dispatcher)
    : m_client(client)
    , m_dispatcher(Dispatcher::create(WTFMove(dispatcher)))
{
}

MediaSourceHandle::MediaSourceHandle(MediaSourceHandle& other)
    : m_client(other.m_client)
    , m_detached(false)
    , m_dispatcher(other.m_dispatcher)
{
    ASSERT(!other.m_detached);
    other.m_detached = true;
}

MediaSourceHandle::~MediaSourceHandle() = default;

Ref<DetachedMediaSourceHandle> MediaSourceHandle::detach()
{
    return adoptRef(*new MediaSourceHandle(*this));
}

}

#endif // MEDIA_SOURCE_IN_WORKERS
