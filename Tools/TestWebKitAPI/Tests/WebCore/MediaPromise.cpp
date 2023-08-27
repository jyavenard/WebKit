/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 * Copyright (C) 2017-2020 Mozilla Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Test.h"

#include <WebCore/MediaPromise.h>
#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WorkQueue.h>
#include <wtf/threads/BinarySemaphore.h>

using namespace WebCore;

namespace TestWebKitAPI {

using TestPromise = MediaPromise<int, double, false>;
using TestPromiseExcl = MediaPromise<int, double, true /* exclusive */>;
using RRValue = TestPromise::ResolveOrRejectValue;

class WorkQueueWithShutdown : public WorkQueue {
public:
    static Ref<WorkQueueWithShutdown> create(const char* name) { return adoptRef(*new WorkQueueWithShutdown(name)); }
    void beginShutdown()
    {
        dispatch([this, strong = Ref { *this }] {
            m_shutdown.store(true);
            m_semaphore.signal();
        });
    }
    void waitUntilShutdown()
    {
        while (!m_shutdown.load())
            m_semaphore.wait();
    }
private:
    WorkQueueWithShutdown(const char* name) : WorkQueue(name, QOS::Default) { }
    Atomic<bool> m_shutdown { false };
    BinarySemaphore m_semaphore;
};

class AutoWorkQueue {
public:
    AutoWorkQueue()
        : m_workQueue(WorkQueueWithShutdown::create("com.apple.WebKit.Test.simple"))
    {
    }

    Ref<WorkQueueWithShutdown> queue() { return m_workQueue; }

    ~AutoWorkQueue()
    {
        m_workQueue->waitUntilShutdown();
    }

private:
    Ref<WorkQueueWithShutdown> m_workQueue;
};

class DelayedResolveOrReject final : public ThreadSafeRefCounted<DelayedResolveOrReject> {
public:
    DelayedResolveOrReject(WorkQueue& workQueue, TestPromise::Private* promise, TestPromise::ResolveOrRejectValue&& value, int iterations)
        : m_workQueue(workQueue)
        , m_promise(promise)
        , m_value(WTFMove(value))
        , m_iterations(iterations)
    {
    }

    void dispatch()
    {
        m_workQueue->dispatch([strongThis = RefPtr { this }] { strongThis->run(); });
    }

    void run()
    {
        m_workQueue->assertIsCurrent();

        Locker lock { m_lock };
        if (!m_promise) {
            // Canceled.
            return;
        }

        if (--m_iterations == 0) {
            m_promise->resolveOrReject(m_value, __func__);
            return;
        }

        dispatch();
    }

    void cancel()
    {
        Locker lock { m_lock };
        m_promise = nullptr;
    }

private:
    Lock m_lock;
    Ref<WorkQueue> m_workQueue;
    RefPtr<TestPromise::Private> m_promise;
    TestPromise::ResolveOrRejectValue m_value;
    int m_iterations;
};

static std::function<void()> doFail()
{
    return [] { EXPECT_TRUE(false); };
}

static auto doFailAndReject(const char* location)
{
    return [location] {
        EXPECT_TRUE(false);
        return TestPromise::createAndReject(0, location);
    };
}

TEST(MediaPromise, BasicResolve)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        TestPromise::createAndResolve(42, __func__)->then(queue, __func__, [queue] (int resolveValue) {
            EXPECT_EQ(resolveValue, 42);
            queue->beginShutdown();
        }, doFail());
    });
}

TEST(MediaPromise, BasicReject)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        TestPromise::createAndReject(42.0, __func__)->then(queue, __func__, doFail(), [queue] (int rejectValue) {
            EXPECT_EQ(rejectValue, 42.0);
            queue->beginShutdown();
        });
    });
}

TEST(MediaPromise, BasicResolveOrRejectResolved)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        TestPromise::createAndResolve(42, __func__)->then(queue, __func__, [queue] (const TestPromise::ResolveOrRejectValue& value) {
            EXPECT_TRUE(value.isResolve());
            EXPECT_FALSE(value.isReject());
            EXPECT_FALSE(value.isNothing());
            EXPECT_EQ(value.resolveValue(), 42);
            queue->beginShutdown();
        });
    });
}

TEST(MediaPromise, BasicResolveOrRejectRejected)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        TestPromise::createAndReject(42.0, __func__)->then(queue, __func__, [queue] (const TestPromise::ResolveOrRejectValue& value) {
            EXPECT_TRUE(value.isReject());
            EXPECT_FALSE(value.isResolve());
            EXPECT_FALSE(value.isNothing());
            EXPECT_EQ(value.rejectValue(), 42.0);
            queue->beginShutdown();
        });
    });
}

TEST(MediaPromise, AsyncResolve)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        RefPtr<TestPromise::Private> p = adoptRef(new TestPromise::Private(__func__));

        // Kick off three racing tasks, and make sure we get the one that finishes
        // earliest.
        auto a = adoptRef(new DelayedResolveOrReject(queue, p.get(), RRValue::makeResolve(32), 10));
        auto b = adoptRef(new DelayedResolveOrReject(queue, p.get(), RRValue::makeResolve(42), 5));
        auto c = adoptRef(new DelayedResolveOrReject(queue, p.get(), RRValue::makeReject(32.0), 7));

        a->dispatch();
        b->dispatch();
        c->dispatch();

        p->then(queue, __func__, [queue, a, b, c] (int resolveValue) {
            EXPECT_EQ(resolveValue, 42);
            a->cancel();
            b->cancel();
            c->cancel();
            queue->beginShutdown();
        }, doFail());
    });
}

TEST(MediaPromise, CompletionPromises)
{
    Atomic<bool> invokedPass { false };
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue, &invokedPass] {
        TestPromise::createAndResolve(40, __func__)
        ->then(queue, __func__, [] (int val) -> Ref<TestPromise> {
            return TestPromise::createAndResolve(val + 10, __func__);
        }, doFailAndReject(__func__))
        ->then(queue, __func__, [&invokedPass] (int val) {
            invokedPass.store(true);
            return TestPromise::createAndResolve(val, __func__);
        }, doFailAndReject(__func__))
        ->then(queue, __func__, [queue] (int val) -> Ref<TestPromise> {
            auto p = adoptRef(*new TestPromise::Private(__func__));
            auto resolver = adoptRef(new DelayedResolveOrReject(queue, p.ptr(), RRValue::makeResolve(val - 8), 10));
            resolver->dispatch();
            return p;
        }, doFailAndReject(__func__))
        ->then(queue, __func__, [] (int val) -> Ref<TestPromise> {
            return TestPromise::createAndReject(double(val - 42) + 42.0, __func__);
        }, doFailAndReject(__func__))
        ->then(queue, __func__, doFail(), [queue, &invokedPass] (double val) {
            EXPECT_EQ(val, 42.0);
            EXPECT_TRUE(invokedPass.load());
            queue->beginShutdown();
        });
    });
}

TEST(MediaPromise, PromiseAllResolve)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        Vector<Ref<TestPromise>> promises;
        promises.append(TestPromise::createAndResolve(22, __func__));
        promises.append(TestPromise::createAndResolve(32, __func__));
        promises.append(TestPromise::createAndResolve(42, __func__));

        TestPromise::all(queue, promises)->then(queue, __func__, [queue] (const Vector<int>& resolveValues) {
            EXPECT_EQ(resolveValues.size(), 3UL);
            EXPECT_EQ(resolveValues[0], 22);
            EXPECT_EQ(resolveValues[1], 32);
            EXPECT_EQ(resolveValues[2], 42);
            queue->beginShutdown();
        }, [] { EXPECT_TRUE(false); });
    });
}

TEST(MediaPromise, PromiseAllResolveAsync)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        Vector<Ref<TestPromise>> promises;
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(22, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(32, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(42, __func__);
        }));

        TestPromise::all(queue, promises)->then(queue, __func__, [queue] (const Vector<int>& resolveValues) {
            EXPECT_EQ(resolveValues.size(), 3UL);
            EXPECT_EQ(resolveValues[0], 22);
            EXPECT_EQ(resolveValues[1], 32);
            EXPECT_EQ(resolveValues[2], 42);
            queue->beginShutdown();
        }, doFail());
    });
}

TEST(MediaPromise, PromiseAllReject)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        Vector<Ref<TestPromise>> promises;
        promises.append(TestPromise::createAndResolve(22, __func__));
        promises.append(TestPromise::createAndReject(32.0, __func__));
        promises.append(TestPromise::createAndResolve(42, __func__));
        // Ensure that more than one rejection doesn't cause a crash
        promises.append(TestPromise::createAndReject(52.0, __func__));

        TestPromise::all(queue, promises)->then(queue, __func__,
            doFail(),
            [queue] (float rejectValue) {
                EXPECT_EQ(rejectValue, 32.0);
                queue->beginShutdown();
            });
    });
}

TEST(MediaPromise, PromiseAllRejectAsync)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        Vector<Ref<TestPromise>> promises;
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(22, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndReject(32.0, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(42, __func__);
        }));
        // Ensure that more than one rejection doesn't cause a crash
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndReject(52.0, __func__);
        }));

        TestPromise::all(queue, promises)->then(queue, __func__,
            doFail(),
            [queue] (float rejectValue) {
                EXPECT_EQ(rejectValue, 32.0);
                queue->beginShutdown();
            });
    });
}

TEST(MediaPromise, PromiseAllSettled)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();
    queue->dispatch([queue] {
        Vector<Ref<TestPromise>> promises;
        promises.append(TestPromise::createAndResolve(22, __func__));
        promises.append(TestPromise::createAndReject(32.0, __func__));
        promises.append(TestPromise::createAndResolve(42, __func__));
        promises.append(TestPromise::createAndReject(52.0, __func__));

        TestPromise::allSettled(queue, promises)->then(queue, __func__,
            [queue] (const TestPromise::AllSettledPromiseType::ResolveValueType& resolveValues) {
                EXPECT_EQ(resolveValues.size(), 4UL);
                EXPECT_TRUE(resolveValues[0].isResolve());
                EXPECT_EQ(resolveValues[0].resolveValue(), 22);
                EXPECT_FALSE(resolveValues[1].isResolve());
                EXPECT_EQ(resolveValues[1].rejectValue(), 32.0);
                EXPECT_TRUE(resolveValues[2].isResolve());
                EXPECT_EQ(resolveValues[2].resolveValue(), 42);
                EXPECT_FALSE(resolveValues[3].isResolve());
                EXPECT_EQ(resolveValues[3].rejectValue(), 52.0);
                queue->beginShutdown();
            }, doFail());
    });
}

TEST(MediaPromise, PromiseAllSettledAsync)
{
    AutoWorkQueue awq;
    auto queue = awq.queue();

    queue->dispatch([queue] {
        Vector<Ref<TestPromise>> promises;
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(22, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndReject(32.0, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndResolve(42, __func__);
        }));
        promises.append(invokeAsync(queue, __func__, [] {
            return TestPromise::createAndReject(52.0, __func__);
        }));

        TestPromise::allSettled(queue, promises)->then(queue, __func__,
            [queue] (const TestPromise::AllSettledPromiseType::ResolveValueType& resolveValues) {
                EXPECT_EQ(resolveValues.size(), 4UL);
                EXPECT_TRUE(resolveValues[0].isResolve());
                EXPECT_EQ(resolveValues[0].resolveValue(), 22);
                EXPECT_FALSE(resolveValues[1].isResolve());
                EXPECT_EQ(resolveValues[1].rejectValue(), 32.0);
                EXPECT_TRUE(resolveValues[2].isResolve());
                EXPECT_EQ(resolveValues[2].resolveValue(), 42);
                EXPECT_FALSE(resolveValues[3].isResolve());
                EXPECT_EQ(resolveValues[3].rejectValue(), 52.0);
                queue->beginShutdown();
            }, doFail());
    });
}

// Test we don't hit the assertions in MediaPromise when exercising promise
// chaining upon task queue shutdown.
TEST(MediaPromise, Chaining)
{
    // We declare this variable before |awq| to ensure
    // the destructor is run after |holder.disconnect()|.
    MediaPromiseRequestHolder<TestPromise> holder;

    AutoWorkQueue awq;
    auto queue = awq.queue();

    queue->dispatch([queue, &holder] {
        auto p = TestPromise::createAndResolve(42, __func__);
        const size_t kIterations = 100;
        for (size_t i = 0; i < kIterations; ++i) {
            p = p->then(queue, __func__,
                        [] (int val) {
                            EXPECT_EQ(val, 42);
                            return TestPromise::createAndResolve(val, __func__);
                        },
                        [] (double val) {
                            return TestPromise::createAndReject(val, __func__);
                        });

            if (i == kIterations / 2)
                p->then(queue, __func__, [queue, &holder] {
                    holder.disconnect();
                    queue->beginShutdown();
                }, doFail());
        }
        // We will hit the assertion if we don't disconnect the leaf Request
        // in the promise chain.
        p->then(queue, __func__, [] {}, [] {})->track(holder);
    });
}

TEST(MediaPromise, ResolveOrRejectValue)
{
    using MyPromise = MediaPromise<std::unique_ptr<int>, bool, false>;
    using RRValue = MyPromise::ResolveOrRejectValue;

    RRValue val;
    EXPECT_TRUE(val.isNothing());
    EXPECT_FALSE(val.isResolve());
    EXPECT_FALSE(val.isReject());

    val.setResolve(std::make_unique<int>(87));
    EXPECT_FALSE(val.isNothing());
    EXPECT_TRUE(val.isResolve());
    EXPECT_FALSE(val.isReject());
    EXPECT_EQ(87, *val.resolveValue());

    // isResolve() should remain true after std::move().
    std::unique_ptr<int> i = WTFMove(val.resolveValue());
    EXPECT_EQ(87, *i);
    EXPECT_TRUE(val.isResolve());
    EXPECT_EQ(val.resolveValue().get(), nullptr);
}

TEST(MediaPromise, MoveOnlyType)
{
    using MyPromise = MediaPromise<std::unique_ptr<int>, bool, true>;
    using RRValue = MyPromise::ResolveOrRejectValue;

    AutoWorkQueue awq;
    auto queue = awq.queue();

    MyPromise::createAndResolve(std::make_unique<int>(87), __func__)
    ->then(queue, __func__,
           [] (std::unique_ptr<int> val) { EXPECT_EQ(87, *val); },
           [] { EXPECT_TRUE(false); });

    MyPromise::createAndResolve(std::make_unique<int>(87), __func__)
    ->then(queue, __func__, [queue] (RRValue&& val) {
        EXPECT_FALSE(val.isNothing());
        EXPECT_TRUE(val.isResolve());
        EXPECT_FALSE(val.isReject());
        EXPECT_EQ(87, *val.resolveValue());

        // std::move() shouldn't change the resolve/reject state of val.
        RRValue val2 = WTFMove(val);
        EXPECT_TRUE(val2.isResolve());
        EXPECT_EQ(nullptr, val.resolveValue().get());
        EXPECT_EQ(87, *val2.resolveValue());
        queue->beginShutdown();
    });
}

TEST(MediaPromise, HeterogeneousChaining)
{
    using Promise1 = MediaPromise<std::unique_ptr<char>, bool, true>;
    using Promise2 = MediaPromise<std::unique_ptr<int>, bool, true>;
    using RRValue1 = Promise1::ResolveOrRejectValue;
    using RRValue2 = Promise2::ResolveOrRejectValue;

    MediaPromiseRequestHolder<Promise2> holder;

    AutoWorkQueue awq;
    auto queue = awq.queue();

    queue->dispatch([queue, &holder] {
        Promise1::createAndResolve(std::make_unique<char>(0), __func__)
        ->then(queue, __func__,
               [&holder] {
            holder.disconnect();
            return Promise2::createAndResolve(std::make_unique<int>(0), __func__);
        })
        ->then(queue, __func__, [] {
            // Shouldn't be called for we've disconnected the request.
            EXPECT_FALSE(true);
        })
        ->track(holder);
    });

    Promise1::createAndResolve(std::make_unique<char>(87), __func__)
    ->then(queue, __func__,
           [] (std::unique_ptr<char> val) {
                EXPECT_EQ(87, *val);
                return Promise2::createAndResolve(std::make_unique<int>(94), __func__);
            }, [] {
                return Promise2::createAndResolve(std::make_unique<int>(95), __func__);
            })
    ->then(queue, __func__,
           [] (std::unique_ptr<int> val) { EXPECT_EQ(94, *val); },
           doFail());

    Promise1::createAndResolve(std::make_unique<char>(87), __func__)
    ->then(queue, __func__, [] (RRValue1&& val) {
        EXPECT_EQ(87, *val.resolveValue());
        return Promise2::createAndResolve(std::make_unique<int>(94), __func__);
    })
    ->then(queue, __func__, [queue] (RRValue2&& val) {
        EXPECT_EQ(94, *val.resolveValue());
        queue->beginShutdown();
    });
}

TEST(MediaPromise, RunLoop)
{
    WTF::initializeMainThread();
    auto& runLoop = RunLoop::current();
    TestPromise::createAndResolve(42, __func__)->then(runLoop, __func__, [] (int resolveValue) { EXPECT_EQ(resolveValue, 42); }, doFail());

    bool done = false;
    runLoop.dispatch([&] {
        done = true;
    });
    while (!done)
        runLoop.cycle();
}

TEST(MediaPromise, ChainTo)
{
    WTF::initializeMainThread();
    auto& runLoop = RunLoop::current();

    auto promise1 = TestPromise::createAndResolve(42, __func__);
    auto promise2 = adoptRef(*new TestPromise::Private(__func__));
    promise2->then(runLoop, __func__, [&] (int resolveValue) { EXPECT_EQ(resolveValue, 42); }, doFail());

    promise1->chainTo(WTFMove(promise2), __func__);

    bool done = false;
    runLoop.dispatch([&] {
        done = true;
    });
    while (!done)
        runLoop.cycle();
}

TEST(MediaPromise, SynchronousFunctionDispatch1)
{
    WTF::initializeMainThread();
    auto& runLoop = RunLoop::current();

    bool value = false;
    auto promise = adoptRef(new TestPromiseExcl::Private(__func__));
    promise->useSynchronousFunctionDispatch(__func__);
    promise->resolve(42, __func__);
    EXPECT_EQ(value, false);
    promise->then(runLoop, __func__,
                  [&] (int resolveValue) {
        EXPECT_EQ(resolveValue, 42);
        value = true;
    }, doFail());
    EXPECT_EQ(value, true);
}

TEST(MediaPromise, SynchronousFunctionDispatch2)
{
    WTF::initializeMainThread();
    auto& runLoop = RunLoop::current();

    bool value = false;
    auto promise = adoptRef(new TestPromiseExcl::Private(__func__));
    promise->useSynchronousFunctionDispatch(__func__);
    promise->then(runLoop, __func__,
        [&] (int resolveValue) {
            EXPECT_EQ(resolveValue, 42);
            value = true;
        }, doFail());
    EXPECT_EQ(value, false);
    promise->resolve(42, __func__);
    EXPECT_EQ(value, true);
}

} // namespace TestWebKitAPI
