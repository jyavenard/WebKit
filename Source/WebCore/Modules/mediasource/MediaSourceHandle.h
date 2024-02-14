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
#include <wtf/Function.h>
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
    static Ref<MediaSourceHandle> create(Ref<MediaSourceHandle>&&);

    virtual ~MediaSourceHandle();

    bool isDetached() const { return m_detached; }
    bool canDetach() const { return !isDetached() && m_hasEverBeenAssignedAsSrcObject; }

    void setHasEverBeenAssignedAsSrcObject() { m_hasEverBeenAssignedAsSrcObject = true; }
    bool hasEverBeenAssignedAsSrcObject() const { return m_hasEverBeenAssignedAsSrcObject; }

    void ensureOnDispatcher(Function<void()>&& task);

    Ref<MediaSourceHandle> detach();

private:
    MediaSourceHandle(MediaSourcePrivateClient&, Function<void(Function<void()>&&)>&&);
    MediaSourceHandle(MediaSourceHandle&);

    class Dispatcher;

    void setDetached(bool value) { m_detached = value; }

    ThreadSafeWeakPtr<MediaSourcePrivateClient> m_client;
    bool m_hasEverBeenAssignedAsSrcObject { false };
    bool m_detached { false };
    Ref<Dispatcher> m_dispatcher;
};

using DetachedMediaSourceHandle = MediaSourceHandle;

} // namespace WebCore

#endif // MEDIA_SOURCE_IN_WORKERS
