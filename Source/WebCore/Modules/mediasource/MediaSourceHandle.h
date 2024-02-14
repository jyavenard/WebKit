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

#pragma once
#if ENABLE(MEDIA_SOURCE_IN_WORKERS)

#include "JSDOMGlobalObject.h"
#include <wtf/Forward.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/ThreadSafeWeakPtr.h>

namespace WebCore {

class MediaSourcePrivate;
class MediaSourcePrivateClient;

class MediaSourceHandle
    : public ThreadSafeRefCounted<MediaSourceHandle>
{
    WTF_MAKE_ISO_ALLOCATED(MediaSourceHandle);
public:
    static Ref<MediaSourceHandle> create(MediaSourcePrivateClient&, Function<void(Function<void()>&&)>&&);
    virtual ~MediaSourceHandle() = default;

    bool detached() const { return m_detached; }
    void ensureOnDispatcher(Function<void()>&& function);
    void setMediaSourcePrivate(MediaSourcePrivate&);

private:
    MediaSourceHandle(MediaSourcePrivateClient&, Function<void(Function<void()>&&)>&&);

    ThreadSafeWeakPtr<MediaSourcePrivate> m_private;
    ThreadSafeWeakPtr<MediaSourcePrivateClient> m_client;
    bool m_hasEverBeenAssigned { false };
    bool m_detached { false };
    const Function<void(Function<void()>&&)> m_dispatcher;
};

} // namespace WebCore

#endif // MEDIA_SOURCE_IN_WORKERS
