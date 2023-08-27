/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 * Copyright (C) 2015-2020 Mozilla Foundation. All rights reserved.
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

#pragma once

#include "Logging.h"
#include "ParametersStorage.h"

#include <type_traits>
#include <utility>
#include <wtf/FunctionDispatcher.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

#include <wtf/RunLoop.h>

namespace WebCore {

extern WTFLogChannel LogMediaPromise;
#define PROMISE_LOG(x, ...) RELEASE_LOG_WITH_LEVEL(MediaPromise, WTFLogLevel::Debug, x, __VA_ARGS__)

namespace detail {

template<typename F>
struct MethodTraitsHelper : MethodTraitsHelper<decltype(&F::operator())> { };
template<typename ThisType, typename Ret, typename... ArgTypes>
struct MethodTraitsHelper<Ret (ThisType::*)(ArgTypes...)> {
    using returnType = Ret;
    static const size_t argSize = sizeof...(ArgTypes);
};
template<typename ThisType, typename Ret, typename... ArgTypes>
struct MethodTraitsHelper<Ret (ThisType::*)(ArgTypes...) const> {
    using returnType = Ret;
    static const size_t argSize = sizeof...(ArgTypes);
};
template<typename ThisType, typename Ret, typename... ArgTypes>
struct MethodTraitsHelper<Ret (ThisType::*)(ArgTypes...) volatile> {
    using returnType = Ret;
    static const size_t argSize = sizeof...(ArgTypes);
};
template<typename ThisType, typename Ret, typename... ArgTypes>
struct MethodTraitsHelper<Ret (ThisType::*)(ArgTypes...) const volatile> {
    using returnType = Ret;
    static const size_t argSize = sizeof...(ArgTypes);
};

template<typename T>
struct MethodTrait : MethodTraitsHelper<std::remove_reference_t<T>> { };

} // namespace detail

template<typename MethodType>
using TakesArgument = std::integral_constant<bool, detail::MethodTrait<MethodType>::argSize != 0>;

template<typename ResolveValueT, typename RejectValueT, bool IsExclusive>
class MediaPromise;

template<typename Return>
struct IsMediaPromise : std::false_type { };

template<typename ResolveValueT, typename RejectValueT, bool IsExclusive>
struct IsMediaPromise<MediaPromise<ResolveValueT, RejectValueT, IsExclusive>> : std::true_type { };

/*
 * A promise manages an asynchronous request that may or may not be able to be
 * fulfilled immediately. When an API returns a promise, the consumer may attach
 * callbacks to be invoked (asynchronously, on a specified thread) when the
 * request is either completed (resolved) or cannot be completed (rejected).
 * Whereas JS promise callbacks are dispatched from Microtask checkpoints,
 * MediaPromises resolution/rejection make a normal round-trip through the provided
 * SerialFunctionDispatcher, which simplifies their ordering semantics relative
 * to other native code.
 *
 * MediaPromises attempt to mirror the spirit of JS Promises to the extent that
 * is possible (and desirable) in C++. While the intent is that MediaPromises
 * feel familiar to programmers who are accustomed to their JS-implemented
 * cousin, we don't shy away from imposing restrictions and adding features that
 * make sense for the use cases we encounter.
 *
 * A MediaPromise is ThreadSafe, and may be ->then()ed on any thread. The then()
 * call accepts either a resolve and reject callbacks or a resolveOrReject one
 * and returns a magic object which will be implicitly converted to a
 * MediaPromise::Request or a MediaPromise object depending on how the return value
 * is used. The magic object serves several purposes for the consumer.
 *
 *   (1) When converting to a MediaPromise::Request, it allows the caller to
 *       cancel the delivery of the resolve/reject value if it has not already
 *       occurred, through the MediaPromiseRequestHolder via disconnect()
 *       (this must be done on the target thread to avoid racing).
 *       The execution of the callbacks will then be avoided.
 *
 *   (2) When converting to a MediaPromise (which is called a completion promise),
 *       it allows promise chaining so ->then() can be called again to attach
 *       more resolve and reject callbacks. If the resolve/reject callback
 *       returns a new MediaPromise, that promise is chained to the completion
 *       promise, such that its resolve/reject value will be forwarded along
 *       when it arrives. If the resolve/reject callback returns void, the
 *       completion promise is resolved/rejected with the same value that was
 *       passed to the callback.
 *
 * When IsExclusive is true, the MediaPromise does a release-mode assertion that
 * there is at most one call to either then(...) or chainTo(...).
 * When IsExclusive is true, move semantics are used when passing arguments whenever
 * possible, otherwise values are passed to the resolve/reject callbacks through
 * either const references or pointers.
 *
 * A MediaPromise attempts to allow for thread-safe transfer of a parameter from one
 * thread to another by storing/moving the object in a safe fashion.
 * See ParameterStorage template.
 *
 * Convenience holders MediaPromiseHolder and MediaPromiseRequestHolder are designed
 * to help facilitate the most common code pattern.
 * MediaPromiseHolder:
 *  - create the promise
 *  - resolve or reject the promise
 * MediaPromiseRequestHolder:
 *  - track the promise
 *  - cancel the delivery of the resolve/reject value and prevent callbacks to be run.
 * WARNING: Neither MediaPromiseHolder nor MediaPromiseRequestHolder are thread-safe
 * (though they may be safely moved).
 *
 * Examples:
 * 1. Basic usage. methodA runs on the main thread, methodB must run on a WorkQueue, and expects a std::unique<int>.
 *    methodA calls methodB for asynchronous work and will perform some work once methodB is done.
 *
 *    static Ref<GenericPromise> methodB(std_unique<int>&& arg)
 *    {
 *        workQueue->assertIsCurrent();
 *        // Do something with arg and once done return a resolved promise.
 *        if (all_ok)
 *            return GenericPromise::createAndResolve(true, __func__);
 *        else
 *            return GenericPromise::createAndReject(false, __func__);
 *    }
 *
 *    static void methodA()
 *    {
 *        assertIsMainThread();
 *        auto arg = std::make_unique<int>(20);
 *        // invokeAsync returns a promise of the same type as the function called,
 *        // and it will be resolved when the original promise is resolved.
 *        invokeAsync(workQueue, __func__ &methodB, WTFMove(arg))
 *        ->then(RunLoop::main(), __func__,
 *            [](bool) {
 *                assertIsMainThread();
 *                // Method succeeded
 *            }, [](bool) {
 *                assertIsMainThread();
 *                // Method failed
 *            });
 *    }
 *
 * 2. Using lambdas
 *    auto p = MyAsyncMethod(); // MyAsyncMethod returns a Ref<MediaPromise>, and perform some work on some thread.
 *    p->then(RunLoop::main(), __func__, [] (MediaPromise::ResolveOrRejectValue&& result) {
 *        assertIsMainThread();
 *        if (result.isResolve()) {
 *            auto resolveValue = WTFMove(result.resolveValue());
 *        } else {
 *            auto rejectValue = WTFMove(result.rejectValue());
 *        }
 *    }
 *
 * 3. Using MediaPromiseHolder and MediaPromiseRequestHolder
 *    MediaPromiseHolder<GenericPromise> promise;
 *    MediaPromiseRequestHolder<GenericPromise> request;
 *
 *    auto p = promise.ensure(__func__);
 *    // Note that if you're not interested with the result you can provide a Function<void()>
 *    p->then(RunLoop::main(), __func__,
 *            [] { CRASH("resolve callback won't be run"); },
 *            [] { CRASH("reject callback won't be run"); })
 *      ->track(request);
 *
 *    // We are no longer interested by the result of the promise. We disconnect the request holder.
 *    request.disconnect();
 *
 * 4. Chaining promises of different types
 *    auto p = MyAsyncMethod(); // MyAsyncMethod returns a Ref<MyMediaPromise>, and perform some work on some thread.
 *    auto p2 = p->then(RunLoop::main(), __func__, [] (MyMediaPromise::ResolveValueType val) {
 *            assertIsMainThread();
 *            if (val)
 *                return MyOtherPromise::createAndResolve(val, __func__);
 *            return MyOtherPromise::createAndReject(val, __func__);
 *        }, [] (MyOtherPromise::RejectValueType val) {
 *            return MyOtherPromise::createAndReject(val, __func__);
 *        }) // The type returned by then() if of the last PromiseType returned in the chain.
 *        ->then(RunLoop::main(), __func__, [] (const MyOtherPromise::ResolveOrRejectValue&) -> void {
 *            // do something else
 *        });
 *
 *
 * For additional examples on how to use MediaPromise, refer to MediaPromise.cpp API tests.
 */

class MediaPromiseBase : public ThreadSafeRefCounted<MediaPromiseBase>  {
public:
    virtual void assertIsDead() = 0;
    virtual ~MediaPromiseBase() = default;
};

template<typename T>
class MediaPromiseHolder;
template<typename T>
class MediaPromiseRequestHolder;

template<typename ResolveValueT, typename RejectValueT, bool IsExclusive>
class MediaPromise : public MediaPromiseBase {

    // Return a |T&&| to enable move when IsExclusive is true or
    // a |const T&| to enforce copy otherwise.
    template<typename T, typename R = std::conditional_t<IsExclusive, T&&, const T&>>
    static R maybeMove(T& aX)
    {
        return static_cast<R>(aX);
    }

public:
    using ResolveValueType = ResolveValueT;
    using RejectValueType = RejectValueT;
    class ResolveOrRejectValue {
    public:
        template<typename ResolveValueType_>
        void setResolve(ResolveValueType_&& resolveValue)
        {
            ASSERT(isNothing());
            m_value = Storage(std::in_place_index<ResolveIndex>,
                std::forward<ResolveValueType_>(resolveValue));
        }

        template<typename RejectValueType_>
        void setReject(RejectValueType_&& rejectValue)
        {
            ASSERT(isNothing());
            m_value = Storage(std::in_place_index<RejectIndex>,
                std::forward<RejectValueType_>(rejectValue));
        }

        template<typename ResolveValueType_>
        static ResolveOrRejectValue makeResolve(ResolveValueType_&& resolveValue)
        {
            ResolveOrRejectValue val;
            val.setResolve(std::forward<ResolveValueType_>(resolveValue));
            return val;
        }

        template<typename RejectValueType_>
        static ResolveOrRejectValue makeReject(RejectValueType_&& rejectValue)
        {
            ResolveOrRejectValue val;
            val.setReject(std::forward<RejectValueType_>(rejectValue));
            return val;
        }

        bool isResolve() const { return m_value.index() == ResolveIndex; }
        bool isReject() const { return m_value.index() == RejectIndex; }
        bool isNothing() const { return m_value.index() == NothingIndex; }

        const ResolveValueType& resolveValue() const
        {
            return std::get<ResolveIndex>(m_value);
        }
        ResolveValueType& resolveValue()
        {
            return std::get<ResolveIndex>(m_value);
        }
        const RejectValueType& rejectValue() const
        {
            return std::get<RejectIndex>(m_value);
        }
        RejectValueType& rejectValue()
        {
            return std::get<RejectIndex>(m_value);
        }

    private:
        enum { NothingIndex, ResolveIndex, RejectIndex };
        struct Nothing { };

        using Storage = std::variant<Nothing, ResolveValueType, RejectValueType>;
        Storage m_value = Storage(std::in_place_index<NothingIndex>);
    };

protected:
    // MediaPromise is the public type, and never constructed directly. Construct
    // a MediaPromise::Private, defined below.
    MediaPromise(const char* creationSite, bool isCompletionPromise)
        : m_creationSite(creationSite)
        , m_haveRequest(false)
        , m_isCompletionPromise(isCompletionPromise)
    {
        PROMISE_LOG("%s creating MediaPromise (%p)", m_creationSite, this);
    }

public:
    // MediaPromise::Private allows us to separate the public interface (upon which
    // consumers of the promise may invoke methods like then()) from the private
    // interface (upon which the creator of the promise may invoke resolve() or
    // reject()). APIs should create and store a MediaPromise::Private (usually
    // via a MediaPromiseHolder), and return a MediaPromise to consumers.
    //
    // NB: We can include the definition of this class inline once B2G ICS is
    // gone.
    class Private;

    template<typename ResolveValueType_>
    [[nodiscard]] static Ref<MediaPromise> createAndResolve(
        ResolveValueType_&& resolveValue, const char* resolveSite)
    {
        static_assert(std::is_convertible_v<ResolveValueType_, ResolveValueT>,
            "resolve() argument must be implicitly convertible to "
            "MediaPromise's ResolveValueT");
        auto p = adoptRef(*new MediaPromise::Private(resolveSite));
        p->resolve(std::forward<ResolveValueType_>(resolveValue), resolveSite);
        return p;
    }

    template<typename RejectValueType_>
    [[nodiscard]] static Ref<MediaPromise> createAndReject(RejectValueType_&& rejectValue, const char* rejectSite)
    {
        static_assert(std::is_convertible_v<RejectValueType_, RejectValueT>, "reject() argument must be implicitly convertible to MediaPromise's RejectValueT");
        auto p = adoptRef(*new MediaPromise::Private(rejectSite));
        p->reject(std::forward<RejectValueType_>(rejectValue), rejectSite);
        return p;
    }

    template<typename ResolveOrRejectValueType_>
    [[nodiscard]] static Ref<MediaPromise> createAndResolveOrReject(ResolveOrRejectValueType_&& value, const char* site)
    {
        auto p = adoptRef(*new MediaPromise::Private(site));
        p->resolveOrReject(std::forward<ResolveOrRejectValueType_>(value), site);
        return p;
    }

    using AllPromiseType = MediaPromise<Vector<ResolveValueType>, RejectValueType, IsExclusive>;
    using AllSettledPromiseType = MediaPromise<Vector<ResolveOrRejectValue>, bool, IsExclusive>;

private:
    template <typename VisibleType>
    class AliasedRefPtr {
    public:
        template <typename RefCountedType>
        AliasedRefPtr(RefCountedType* ptr)
            : m_ptr(ptr)
            , m_ptrDeref([](VisibleType* d) {
                static_cast<RefCountedType*>(d)->deref();
            })
        {
            static_assert(HasRefCountMethods<RefCountedType>::value, "Must be used with a RefCounted object");
            ASSERT(ptr);
            ptr->ref();
        }

        AliasedRefPtr(const AliasedRefPtr&) = delete;
        AliasedRefPtr(AliasedRefPtr&& other)
            : m_ptr(std::exchange(other.m_ptr, nullptr))
            , m_ptrDeref(other.m_ptrDeref)
        {
        }

        ~AliasedRefPtr()
        {
            if (m_ptr)
                m_ptrDeref(m_ptr);
        }
        AliasedRefPtr& operator=(const AliasedRefPtr&) = delete;
        AliasedRefPtr& operator=(AliasedRefPtr&& other)
        {
            if (&other == this)
                return;
            m_ptr = std::exchange(other.m_ptr, nullptr);
            m_ptrDeref = other.m_ptrDeref;
            return *this;
        }

        VisibleType* operator->() { return m_ptr; }
    private:
        VisibleType* m_ptr;
        using PtrDeref = void (*)(VisibleType*);
        const PtrDeref m_ptrDeref;
    };
    using ManagedSerialFunctionDispatcher = AliasedRefPtr<SerialFunctionDispatcher>;

    class AllPromiseHolder : public ThreadSafeRefCounted<AllPromiseHolder> {
    public:
        explicit AllPromiseHolder(size_t dependentPromises)
            : m_promise(adoptRef(new typename AllPromiseType::Private(__func__)))
            , m_outstandingPromises(dependentPromises)
        {
            ASSERT(dependentPromises);
            m_resolveValues.resize(dependentPromises);
        }

        template<typename ResolveValueType_>
        void resolve(size_t index, ResolveValueType_&& resolveValue)
        {
            if (!m_promise) {
                // Already resolved or rejected.
                return;
            }

            m_resolveValues[index] = std::forward<ResolveValueType_>(resolveValue);
            if (--m_outstandingPromises == 0) {
                Vector<ResolveValueType> resolveValues;
                resolveValues.reserveInitialCapacity(m_resolveValues.size());
                for (auto&& resolveValue : m_resolveValues)
                    resolveValues.append(WTFMove(resolveValue.value()));

                m_promise->resolve(WTFMove(resolveValues), __func__);
                m_promise = nullptr;
                m_resolveValues.clear();
            }
        }

        template<typename RejectValueType_>
        void reject(RejectValueType_&& rejectValue)
        {
            if (!m_promise) {
                // Already resolved or rejected.
                return;
            }

            m_promise->reject(std::forward<RejectValueType_>(rejectValue), __func__);
            m_promise = nullptr;
            m_resolveValues.clear();
        }

        RefPtr<AllPromiseType> promise() { return m_promise; }

    private:
        Vector<std::optional<ResolveValueType>> m_resolveValues;
        RefPtr<typename AllPromiseType::Private> m_promise;
        size_t m_outstandingPromises;
    };

    // Trying to pass ResolveOrRejectValue by value fails static analysis checks,
    // so we need to use either a const& or an rvalue reference, depending on
    // whether IsExclusive is true or not.
    using ResolveOrRejectValueParam = std::conditional_t<IsExclusive, ResolveOrRejectValue&&, const ResolveOrRejectValue&>;
    using ResolveValueTypeParam = std::conditional_t<IsExclusive, ResolveValueType&&, const ResolveValueType&>;
    using RejectValueTypeParam = std::conditional_t<IsExclusive, RejectValueType&&, const RejectValueType&>;

    class AllSettledPromiseHolder : public RefCounted<AllSettledPromiseHolder> {
    public:
        explicit AllSettledPromiseHolder(size_t dependentPromises)
            : m_promise(new typename AllSettledPromiseType::Private(__func__))
            , m_outstandingPromises(dependentPromises)
        {
            ASSERT(dependentPromises > 0);
            m_values.resize(dependentPromises);
        }

        void settle(size_t index, ResolveOrRejectValueParam value)
        {
            if (!m_promise) {
                // Already settled.
                return;
            }

            m_values[index].emplace(maybeMove(value));
            if (--m_outstandingPromises == 0) {
                Vector<ResolveOrRejectValue> values;
                values.reserveInitialCapacity(m_values.size());
                for (auto&& value : m_values) {
                    values.append(WTFMove(value.value()));
                }

                m_promise->resolve(WTFMove(values), __func__);
                m_promise = nullptr;
                m_values.clear();
            }
        }

        RefPtr<AllSettledPromiseType> promise() { return m_promise; }

    private:
        Vector<std::optional<ResolveOrRejectValue>> m_values;
        RefPtr<typename AllSettledPromiseType::Private> m_promise;
        size_t m_outstandingPromises;
    };

public:
    template <class Dispatcher>
    [[nodiscard]] static Ref<AllPromiseType> all(Dispatcher& processingTarget, Vector<Ref<MediaPromise>>& promises)
    {
        static_assert(LooksLikeRCSerialDispatcher<typename RemoveSmartPointer<Dispatcher>::type>::value, "Must be used with a RefCounted SerialFunctionDispatcher");
        if (promises.isEmpty())
            return AllPromiseType::createAndResolve(Vector<ResolveValueType>(), __func__);

        auto holder = adoptRef(new AllPromiseHolder(promises.size()));
        auto promise = holder->promise();
        for (size_t i = 0; i < promises.size(); ++i) {
            promises[i]->then(processingTarget, __func__,
                [holder, i](ResolveValueTypeParam resolveValue) {
                    holder->resolve(i, maybeMove(resolveValue));
                },
                [holder](RejectValueTypeParam rejectValue) {
                    holder->reject(maybeMove(rejectValue));
                });
        }
        return promise.releaseNonNull();
    }

    template <class Dispatcher>
    [[nodiscard]] static Ref<AllSettledPromiseType> allSettled(Dispatcher& processingTarget, Vector<Ref<MediaPromise>>& promises)
    {
        static_assert(LooksLikeRCSerialDispatcher<typename RemoveSmartPointer<Dispatcher>::type>::value, "Must be used with a RefCounted SerialFunctionDispatcher");
        if (promises.isEmpty())
            return AllSettledPromiseType::createAndResolve(Vector<ResolveOrRejectValue>(), __func__);

        auto holder = adoptRef(new AllSettledPromiseHolder(promises.size()));
        auto promise = holder->promise();
        for (size_t i = 0; i < promises.size(); ++i) {
            promises[i]->then(processingTarget, __func__,
                [holder, i](ResolveOrRejectValueParam value) -> void {
                    holder->settle(i, maybeMove(value));
                });
        }
        return promise.releaseNonNull();
    }

    class Request : public ThreadSafeRefCounted<Request> {
    public:
        virtual void disconnect() = 0;
        virtual ~Request() = default;

    protected:
        Request() = default;

        bool m_complete { false }; // only access on target thread
        bool m_disconnected { false }; // only access on target thread
    };

protected:
    /*
     * A ThenValue tracks a single consumer waiting on the promise. When a
     * consumer invokes promise->then(...), a ThenValue is created. Once the
     * Promise is resolved or rejected, a {Resolve,Reject}Runnable is dispatched,
     * which invokes the resolve/reject method and then deletes the ThenValue.
     */
    class ThenValueBase : public Request {
        friend class MediaPromise;

    public:
        class ResolveOrRejectRunnable final {
        public:
            ResolveOrRejectRunnable(ThenValueBase& thenValue, MediaPromise& promise)
                : m_thenValue(&thenValue)
                , m_promise(promise)
            {
                ASSERT(!m_promise->isPending());
            }

            ResolveOrRejectRunnable(const ResolveOrRejectRunnable&) = delete;
            ResolveOrRejectRunnable(ResolveOrRejectRunnable&& other) = default;
            ResolveOrRejectRunnable& operator=(ResolveOrRejectRunnable&& other) = default;

            ~ResolveOrRejectRunnable()
            {
                if (m_thenValue)
                    m_thenValue->assertIsDead();
            }

            void run()
            {
                PROMISE_LOG("resolveOrRejectRunnable::run() [this=%p]", this);
                m_thenValue->doResolveOrReject(m_promise->value());
                m_thenValue = nullptr;
            }

        private:
            RefPtr<ThenValueBase> m_thenValue; // set at creation, only accessed on target thread.
            Ref<MediaPromise> m_promise; // set at creation, only accessed on target thread.
        };

        ThenValueBase(ManagedSerialFunctionDispatcher&& responseTarget, const char* callSite)
            : m_responseTarget(WTFMove(responseTarget))
            , m_callSite(callSite)
        {
        }

        void assertIsDead()
        {
            // We want to assert that this ThenValues is dead - that is to say, that
            // there are no consumers waiting for the result. In the case of a normal
            // ThenValue, we check that it has been disconnected, which is the way
            // that the consumer signals that it no longer wishes to hear about the
            // result. If this ThenValue has a completion promise (which is mutually
            // exclusive with being disconnectable), we recursively assert that every
            // ThenValue associated with the completion promise is dead.
#if ASSERT_ENABLED
            if (auto* p = completionPromise())
                p->assertIsDead();
            else
                ASSERT(Request::m_disconnected);
#endif
        }

        void dispatch(MediaPromise& promise)
        {
            assertIsHeld(promise.m_lock);
            ASSERT(!promise.isPending());

            ResolveOrRejectRunnable r { *this, promise };
            PROMISE_LOG("%s then() call made from %s [Task=%p, Promise=%p, ThenValue=%p] %s dispatch", promise.m_value.isResolve() ? "Resolving" : "Rejecting", m_callSite, &r, &promise, this, promise.m_useSynchronousFunctionDispatch ? "synchronous" : "normal");

            if (promise.m_useSynchronousFunctionDispatch && m_responseTarget->isCurrent()) {
                PROMISE_LOG("ThenValue::Dispatch running task synchronously [this=%p]", this);
                r.run();
                return;
            }

            m_responseTarget->dispatch([r = WTFMove(r)] () mutable { r.run(); });
        }

        void disconnect() override
        {
            m_responseTarget->assertIsCurrent();
            ASSERT(!Request::m_complete);
            Request::m_disconnected = true;

            // We could support rejecting the completion promise on disconnection, but
            // then we'd need to have some sort of default reject value. The use cases
            // of disconnection and completion promise chaining seem pretty
            // orthogonal, so let's use assert against it.
            ASSERT(!completionPromise());
        }

    protected:
#if ASSERT_ENABLED
        virtual MediaPromiseBase* completionPromise() const = 0; // use for assertions only.
#endif
        virtual void doResolveOrRejectInternal(ResolveOrRejectValue& value) = 0; // only called on target thread.

        void doResolveOrReject(ResolveOrRejectValue& value)
        {
            m_responseTarget->assertIsCurrent();
            Request::m_complete = true;
            if (Request::m_disconnected) {
                PROMISE_LOG("ThenValue::doResolveOrReject disconnected - bailing out [this=%p]", this);
                return;
            }

            // Invoke the resolve or reject method.
            doResolveOrRejectInternal(value);
        }

        ManagedSerialFunctionDispatcher m_responseTarget; // May be released on any thread (safe)
        const char* m_callSite;
    };

    template<typename ThisType, typename MethodType, typename ValueType>
    static typename detail::MethodTrait<MethodType>::returnType invokeMethod(ThisType* thisVal, MethodType method, ValueType&& value)
    {
        // We allow resolve/reject value argument to resolve/reject methods to be "optional"
        if constexpr (TakesArgument<MethodType>::value)
            return (thisVal->*method)(std::forward<ValueType>(value));
        else
            return (thisVal->*method)();
    }

    template<bool SupportChaining, typename ThisType, typename MethodType, typename ValueType, typename CompletionPromiseType>
    static void invokeCallbackMethod(ThisType* thisVal, MethodType method, ValueType&& value, CompletionPromiseType&& completionPromise)
    {
        if constexpr (SupportChaining) {
            auto p = invokeMethod(thisVal, method, std::forward<ValueType>(value));
            if (completionPromise)
                p->chainTo(WTFMove(completionPromise.releaseNonNull()), "<chained completion promise>");
        } else {
            ASSERT(!completionPromise, "Can't do promise chaining for a non-promise-returning method.");
            invokeMethod(thisVal, method, std::forward<ValueType>(value));
        }
    }

    template<typename>
    class ThenCommand;

    template<typename...>
    class ThenValue;

    template<typename ThisType, typename ResolveMethodType, typename RejectMethodType>
    class ThenValue<ThisType*, ResolveMethodType, RejectMethodType> : public ThenValueBase {
        friend class ThenCommand<ThenValue>;

        using R1 = typename RemoveSmartPointer<typename detail::MethodTrait<ResolveMethodType>::returnType>::type;
        using R2 = typename RemoveSmartPointer<typename detail::MethodTrait<RejectMethodType>::returnType>::type;
        using SupportChaining = std::integral_constant<bool, IsMediaPromise<R1>::value && std::is_same_v<R1, R2>>;

        // Fall back to MediaPromise when promise chaining is not supported to make
        // code compile.
        using PromiseType = std::conditional_t<SupportChaining::value, R1, MediaPromise>;

    public:
        ThenValue(ManagedSerialFunctionDispatcher&& responseTarget, ThisType* thisVal, ResolveMethodType resolveMethod, RejectMethodType rejectMethod, const char* callSite)
            : ThenValueBase(WTFMove(responseTarget), callSite)
            , m_thisVal(thisVal)
            , m_resolveMethod(resolveMethod)
            , m_rejectMethod(rejectMethod)
        {
        }

        void disconnect() override
        {
            ThenValueBase::disconnect();

            // If a Request has been disconnected, we don't guarantee that the
            // resolve/reject runnable will be dispatched. Null out our refcounted
            // this-value now so that it's released predictably on the dispatch
            // thread.
            m_thisVal = nullptr;
        }

    protected:
#if ASSERT_ENABLED
        MediaPromiseBase* completionPromise() const override
        {
            return m_completionPromise.get();
        }
#endif
        void doResolveOrRejectInternal(ResolveOrRejectValue& value) override
        {
            if (value.isResolve())
                invokeCallbackMethod<SupportChaining::value>(&m_thisVal.value(), m_resolveMethod, maybeMove(value.resolveValue()), WTFMove(m_completionPromise));
            else
                invokeCallbackMethod<SupportChaining::value>(&m_thisVal.value(), m_rejectMethod, maybeMove(value.rejectValue()), WTFMove(m_completionPromise));

            // Null out m_thisVal after invoking the callback so that any references
            // are released predictably on the dispatch thread. Otherwise, it would be
            // released on whatever thread last drops its reference to the ThenValue,
            // which may or may not be ok.
            m_thisVal = nullptr;
        }

    private:
        RefPtr<ThisType> m_thisVal; // Only accessed and refcounted on dispatch thread.
        ResolveMethodType m_resolveMethod;
        RejectMethodType m_rejectMethod;
        RefPtr<typename PromiseType::Private> m_completionPromise;
    };

    template<typename ThisType, typename ResolveRejectMethodType>
    class ThenValue<ThisType*, ResolveRejectMethodType> : public ThenValueBase {
        friend class ThenCommand<ThenValue>;

        using R1 = typename RemoveSmartPointer<typename detail::MethodTrait<ResolveRejectMethodType>::returnType>::type;
        using SupportChaining = std::integral_constant<bool, IsMediaPromise<R1>::value>;

        // Fall back to MediaPromise when promise chaining is not supported to make code compile.
        using PromiseType = std::conditional_t<SupportChaining::value, R1, MediaPromise>;

    public:
        ThenValue(ManagedSerialFunctionDispatcher&& responseTarget, ThisType* thisVal, ResolveRejectMethodType resolveRejectMethod, const char* callSite)
            : ThenValueBase(WTFMove(responseTarget), callSite)
            , m_thisVal(thisVal)
            , m_resolveRejectMethod(resolveRejectMethod)
        {
        }

        void disconnect() override
        {
            ThenValueBase::disconnect();

            // If a Request has been disconnected, we don't guarantee that the
            // resolve/reject runnable will be dispatched. Null out our refcounted
            // this-value now so that it's released predictably on the dispatch
            // thread.
            m_thisVal = nullptr;
        }

    protected:
#if ASSERT_ENABLED
        MediaPromiseBase* completionPromise() const override
        {
            return m_completionPromise.get();
        }
#endif
        void doResolveOrRejectInternal(ResolveOrRejectValue& value) override
        {
            invokeCallbackMethod<SupportChaining::value>(&m_thisVal.value(), m_resolveRejectMethod, maybeMove(value), WTFMove(m_completionPromise));

            // Null out mThisVal after invoking the callback so that any references
            // are released predictably on the dispatch thread. Otherwise, it would be
            // released on whatever thread last drops its reference to the ThenValue,
            // which may or may not be ok.
            m_thisVal = nullptr;
        }

    private:
        RefPtr<ThisType> m_thisVal; // Only accessed and refcounted on dispatch thread.
        ResolveRejectMethodType m_resolveRejectMethod;
        RefPtr<typename PromiseType::Private> m_completionPromise;
    };

    // NB: We could use std::function here instead of a template if it were
    // supported. :-(
    template<typename ResolveFunction, typename RejectFunction>
    class ThenValue<ResolveFunction, RejectFunction> : public ThenValueBase {
        friend class ThenCommand<ThenValue>;

        using R1 = typename RemoveSmartPointer<typename detail::MethodTrait<ResolveFunction>::returnType>::type;
        using R2 = typename RemoveSmartPointer<typename detail::MethodTrait<RejectFunction>::returnType>::type;
        using SupportChaining = std::integral_constant<bool, IsMediaPromise<R1>::value && std::is_same_v<R1, R2>>;

        // Fall back to MediaPromise when promise chaining is not supported to make code compile.
        using PromiseType = std::conditional_t<SupportChaining::value, R1, MediaPromise>;

    public:
        ThenValue(ManagedSerialFunctionDispatcher&& responseTarget, ResolveFunction&& resolveFunction, RejectFunction&& rejectFunction, const char* callSite)
            : ThenValueBase(WTFMove(responseTarget), callSite)
        {
            m_resolveFunction.emplace(WTFMove(resolveFunction));
            m_rejectFunction.emplace(WTFMove(rejectFunction));
        }

        void disconnect() override
        {
            ThenValueBase::disconnect();

            // If a Request has been disconnected, we don't guarantee that the
            // resolve/reject runnable will be dispatched. Destroy our callbacks
            // now so that any references in closures are released predictable on
            // the dispatch thread.
            m_resolveFunction.reset();
            m_rejectFunction.reset();
        }

    protected:
#if ASSERT_ENABLED
        MediaPromiseBase* completionPromise() const override
        {
            return m_completionPromise.get();
        }
#endif
        void doResolveOrRejectInternal(ResolveOrRejectValue& value) override
        {
            // Note: The usage of invokeCallbackMethod here requires that
            // ResolveFunction/RejectFunction are capture-lambdas (i.e. anonymous
            // classes with ::operator()), since it allows us to share code more
            // easily. We could fix this if need be, though it's quite easy to work
            // around by just capturing something.
            if (value.isResolve())
                invokeCallbackMethod<SupportChaining::value>(&m_resolveFunction.value(), &ResolveFunction::operator(), maybeMove(value.resolveValue()), WTFMove(m_completionPromise));
            else
                invokeCallbackMethod<SupportChaining::value>(&m_rejectFunction.value(), &RejectFunction::operator(), maybeMove(value.rejectValue()), WTFMove(m_completionPromise));

            // Destroy callbacks after invocation so that any references in closures
            // are released predictably on the dispatch thread. Otherwise, they would
            // be released on whatever thread last drops its reference to the
            // ThenValue, which may or may not be ok.
            m_resolveFunction.reset();
            m_rejectFunction.reset();
        }

    private:
        std::optional<ResolveFunction> m_resolveFunction; // Only accessed and deleted on dispatch thread.
        std::optional<RejectFunction> m_rejectFunction; // Only accessed and deleted on dispatch thread.
        RefPtr<typename PromiseType::Private> m_completionPromise;
    };

    template<typename ResolveRejectFunction>
    class ThenValue<ResolveRejectFunction> : public ThenValueBase {
        friend class ThenCommand<ThenValue>;

        using R1 = typename RemoveSmartPointer<typename detail::MethodTrait<ResolveRejectFunction>::returnType>::type;
        using SupportChaining = std::integral_constant<bool, IsMediaPromise<R1>::value>;

        // Fall back to MediaPromise when promise chaining is not supported to make code compile.
        using PromiseType = std::conditional_t<SupportChaining::value, R1, MediaPromise>;

    public:
        ThenValue(ManagedSerialFunctionDispatcher&& responseTarget, ResolveRejectFunction&& resolveRejectFunction, const char* callSite)
            : ThenValueBase(WTFMove(responseTarget), callSite)
        {
            m_resolveRejectFunction.emplace(WTFMove(resolveRejectFunction));
        }

        void disconnect() override
        {
            ThenValueBase::disconnect();

            // If a Request has been disconnected, we don't guarantee that the
            // resolve/reject runnable will be dispatched. Destroy our callbacks
            // now so that any references in closures are released predictable on
            // the dispatch thread.
            m_resolveRejectFunction.reset();
        }

    protected:
#if ASSERT_ENABLED
        MediaPromiseBase* completionPromise() const override
        {
            return m_completionPromise.get();
        }
#endif
        void doResolveOrRejectInternal(ResolveOrRejectValue& value) override
        {
            // Note: The usage of invokeCallbackMethod here requires that
            // ResolveRejectFunction is capture-lambdas (i.e. anonymous
            // classes with ::operator()), since it allows us to share code more
            // easily. We could fix this if need be, though it's quite easy to work
            // around by just capturing something.
            invokeCallbackMethod<SupportChaining::value>(&m_resolveRejectFunction.value(), &ResolveRejectFunction::operator(), maybeMove(value), WTFMove(m_completionPromise));

            // Destroy callbacks after invocation so that any references in closures
            // are released predictably on the dispatch thread. Otherwise, they would
            // be released on whatever thread last drops its reference to the
            // ThenValue, which may or may not be ok.
            m_resolveRejectFunction.reset();
        }

    private:
        std::optional<ResolveRejectFunction> m_resolveRejectFunction; // Only accessed and deleted on dispatch thread.
        RefPtr<typename PromiseType::Private> m_completionPromise;
    };

public:
    void thenInternal(RefPtr<ThenValueBase>&& thenValue, const char* callSite)
    {
        Locker lock { m_lock };
        ASSERT(!IsExclusive || !m_haveRequest, "Using an exclusive promise in a non-exclusive fashion");
        m_haveRequest = true;
        PROMISE_LOG("%s invoking then() [this=%p, thenValue=%p, isPending=%d]", callSite, this, thenValue.get(), (int)isPending());
        if (!isPending())
            thenValue->dispatch(*this);
        else
            m_thenValues.append(WTFMove(thenValue));
    }

protected:
    /*
     * A command object to store all information needed to make a request to
     * the promise. This allows us to delay the request until further use is
     * known (whether it is ->then() again for more promise chaining or ->track()
     * to terminate chaining and issue the request).
     *
     * This allows a unified syntax for promise chaining and disconnection
     * and feels more like its JS counterpart.
     */
    template<typename ThenValueType>
    class ThenCommand {
        // Allow Promise1::ThenCommand to access the private constructor,
        // Promise2::ThenCommand(ThenCommand&&).
        template<typename, typename, bool>
        friend class MediaPromise;

        using PromiseType = typename ThenValueType::PromiseType;
        using Private = typename PromiseType::Private;

        ThenCommand(const char* callSite, RefPtr<ThenValueType>&& thenValue, MediaPromise& receiver)
            : m_callSite(callSite)
            , m_thenValue(WTFMove(thenValue))
            , m_receiver(receiver)
        {
        }

        ThenCommand(ThenCommand&& other) = default;
        ThenCommand& operator=(ThenCommand&& other) = default;

    public:
        ~ThenCommand()
        {
            // Issue the request now if the return value of then() is not used.
            if (m_thenValue)
                m_receiver->thenInternal(WTFMove(m_thenValue), m_callSite);
        }

        // Allow Ref<MediaPromise> p = somePromise->then();
        //       p->then(thread1, ...);
        //       p->then(thread2, ...);
        operator Ref<PromiseType>()
        {
            static_assert(ThenValueType::SupportChaining::value, "The resolve/reject callback needs to return a Ref<MediaPromise> in order to do promise chaining.");

            // m_completionPromise must be created before thenInternal() to avoid race.
            auto p = adoptRef(*new Private("<completion promise>", true /* isCompletionPromise */));
            m_thenValue->m_completionPromise = p.ptr();
            // Note thenInternal() might nullify m_completionPromise before return.
            // So we need to return p instead of m_completionPromise.
            m_receiver->thenInternal(WTFMove(m_thenValue), m_callSite);
            return p;
        }

        template<typename... Ts>
        auto then(Ts&&... args) -> decltype(std::declval<PromiseType>().then(std::forward<Ts>(args)...))
        {
            return static_cast<Ref<PromiseType>>(*this)->then(std::forward<Ts>(args)...);
        }

        void track(MediaPromiseRequestHolder<MediaPromise>& requestHolder)
        {
            requestHolder.track(m_thenValue);
            m_receiver->thenInternal(WTFMove(m_thenValue), m_callSite);
        }

        // Allow calling ->then() again for more promise chaining or ->track() to
        // end chaining and track the request for future disconnection.
        ThenCommand* operator->() { return this; }

    private:
        const char* m_callSite;
        RefPtr<ThenValueType> m_thenValue;
        Ref<MediaPromise> m_receiver;
    };

public:
    template<class DispatcherType, typename ThisType, typename... Methods, typename ThenValueType = ThenValue<ThisType*, Methods...>, typename ReturnType = ThenCommand<ThenValueType>>
    ReturnType then(DispatcherType& responseTarget, const char* callSite, ThisType* thisVal, Methods... methods)
    {
        using DispatcherRealType = typename RemoveSmartPointer<DispatcherType>::type;
        static_assert(LooksLikeRCSerialDispatcher<DispatcherRealType>::value, "Must be used with a RefCounted SerialFunctionDispatcher");
        ManagedSerialFunctionDispatcher dispatcher { &static_cast<DispatcherRealType&>(responseTarget) };
        auto thenValue = adoptRef(new ThenValueType(WTFMove(dispatcher), thisVal, methods..., callSite));
        return ReturnType(callSite, WTFMove(thenValue), *this);
    }

    template<class DispatcherType, typename... Functions, typename ThenValueType = ThenValue<Functions...>, typename ReturnType = ThenCommand<ThenValueType>>
    ReturnType then(DispatcherType& responseTarget, const char* callSite, Functions&&... functions)
    {
        using DispatcherRealType = typename RemoveSmartPointer<DispatcherType>::type;
        static_assert(LooksLikeRCSerialDispatcher<DispatcherRealType>::value, "Must be used with a RefCounted SerialFunctionDispatcher");
        ManagedSerialFunctionDispatcher dispatcher { &static_cast<DispatcherRealType&>(responseTarget) };
        auto thenValue = adoptRef(new ThenValueType(WTFMove(dispatcher), WTFMove(functions)..., callSite));
        return ReturnType(callSite, WTFMove(thenValue), *this);
    }

    void chainTo(Ref<Private>&& chainedPromise, const char* callSite)
    {
        Locker lock { m_lock };
        ASSERT(!IsExclusive || !m_haveRequest, "Using an exclusive promise in a non-exclusive fashion");
        m_haveRequest = true;
        PROMISE_LOG("%s invoking Chain() [this=%p, chainedPromise=%p, isPending=%d]", callSite, this, chainedPromise.ptr(), (int)isPending());

        // We want to use the same type of dispatching method with the chained
        // promises.

        // We need to ensure that the useSynchronousFunctionDispatch branch isn't taken
        // at compilation time to ensure we're not triggering the static_assert in
        // useSynchronousTaskDispatch method. if constexpr (IsExclusive) ensures
        // that.
        if constexpr (IsExclusive) {
            if (m_useSynchronousFunctionDispatch)
                chainedPromise->useSynchronousFunctionDispatch(callSite);
        }
        if (!isPending())
            forwardTo(WTFMove(chainedPromise));
        else
            m_chainedPromises.append(WTFMove(chainedPromise));
    }

    void assertIsDead() final
    {
        Locker lock { m_lock };
        for (auto&& then : m_thenValues)
            then->assertIsDead();
        for (auto&& chained : m_chainedPromises)
            chained->assertIsDead();
    }

    bool isResolved() const { return m_value.isResolve(); }

    ~MediaPromise()
    {
        PROMISE_LOG("MediaPromise::~MediaPromise [this=%p]", this);
        assertIsDead();
        // We can't guarantee a completion promise will always be revolved or
        // rejected since resolveOrRejectRunnable might not run when dispatch fails.
        if (!m_isCompletionPromise) {
            ASSERT(!isPending());
            ASSERT(m_thenValues.isEmpty());
            ASSERT(m_chainedPromises.isEmpty());
        }
    };

protected:
    bool isPending() const { return m_value.isNothing(); }

    ResolveOrRejectValue& value()
    {
        // This method should only be called once the value has stabilized. As such, we don't need to acquire the lock here.
        ASSERT(!isPending());
        return m_value;
    }

    void dispatchAll()
    {
        assertIsHeld(m_lock);
        for (auto&& thenValue : m_thenValues) {
            thenValue->dispatch(*this);
        }
        m_thenValues.clear();

        for (auto&& chainedPromise : m_chainedPromises) {
            forwardTo(WTFMove(chainedPromise));
        }
        m_chainedPromises.clear();
    }

    void forwardTo(Ref<Private>&& other)
    {
        ASSERT(!isPending());
        if (m_value.isResolve()) {
            other->resolve(maybeMove(m_value.resolveValue()), "<chained promise>");
        } else {
            other->reject(maybeMove(m_value.rejectValue()), "<chained promise>");
        }
    }

    const char* m_creationSite; // For logging
    Lock m_lock;
    ResolveOrRejectValue m_value; // Only set/read while holding m_lock on caller thread, then once set only read on the target thread.
    bool m_useSynchronousFunctionDispatch WTF_GUARDED_BY_LOCK(m_lock) { false };
    // Try shows we never have more than 3 elements when IsExclusive is false.
    // So '3' is a good value to avoid heap allocation in most cases.
    Vector<RefPtr<ThenValueBase>, IsExclusive ? 1 : 3> m_thenValues WTF_GUARDED_BY_LOCK(m_lock);
    Vector<Ref<Private>> m_chainedPromises WTF_GUARDED_BY_LOCK(m_lock);
    bool m_haveRequest WTF_GUARDED_BY_LOCK(m_lock);
    const bool m_isCompletionPromise;
};

template<typename ResolveValueT, typename RejectValueT, bool IsExclusive>
class MediaPromise<ResolveValueT, RejectValueT, IsExclusive>::Private
    : public MediaPromise<ResolveValueT, RejectValueT, IsExclusive> {
public:
    explicit Private(const char* creationSite, bool isCompletionPromise = false)
        : MediaPromise(creationSite, isCompletionPromise)
    {
    }

    template<typename ResolveValueT_>
    void resolve(ResolveValueT_&& resolveValue, const char* resolveSite)
    {
        Locker lock { m_lock };
        PROMISE_LOG("%s resolving MediaPromise (%p created at %s)", resolveSite, this, m_creationSite);
        if (!isPending()) {
            PROMISE_LOG("%s ignored already resolved or rejected MediaPromise (%p created at %s)", resolveSite, this, m_creationSite);
            return;
        }
        m_value.setResolve(std::forward<ResolveValueT_>(resolveValue));
        dispatchAll();
    }

    template<typename RejectValueT_>
    void reject(RejectValueT_&& rejectValue, const char* rejectSite)
    {
        Locker lock { m_lock };
        PROMISE_LOG("%s rejecting MediaPromise (%p created at %s)", rejectSite, this, m_creationSite);
        if (!isPending()) {
            PROMISE_LOG("%s ignored already resolved or rejected MediaPromise (%p created at %s)", rejectSite, this, m_creationSite);
            return;
        }
        m_value.setReject(std::forward<RejectValueT_>(rejectValue));
        dispatchAll();
    }

    template<typename ResolveOrRejectValue_>
    void resolveOrReject(ResolveOrRejectValue_&& value, const char* site)
    {
        Locker lock { m_lock };
        PROMISE_LOG("%s resolveOrRejecting MediaPromise (%p created at %s)", site, this, m_creationSite);
        if (!isPending()) {
            PROMISE_LOG("%s ignored already resolved or rejected MediaPromise (%p created at %s)", site, this, m_creationSite);
            return;
        }
        m_value = std::forward<ResolveOrRejectValue_>(value);
        dispatchAll();
    }

    // If the caller and target are both on the same thread, run the the resolve
    // or reject callback synchronously. Otherwise, the task will be dispatched
    // via the target Dispatch method.
    void useSynchronousFunctionDispatch(const char* site)
    {
        static_assert(IsExclusive, "Synchronous dispatch can only be used with exclusive promises");
        Locker lock { m_lock };
        PROMISE_LOG("%s useSynchronousFunctionDispatch MediaPromise (%p created at %s)", site, this, m_creationSite);
        ASSERT(isPending(), "A Promise must not have been already resolved or rejected to set dispatch state");
        m_useSynchronousFunctionDispatch = true;
    }
};

// A generic promise type that does the trick for simple use cases.
using GenericPromise = MediaPromise<bool, bool, /* IsExclusive = */ true>;

// A generic, non-exclusive promise type that does the trick for simple use cases.
using GenericNonExclusivePromise = MediaPromise<bool, bool, /* IsExclusive = */ false>;

/*
 * Class to encapsulate a promise for a particular role. Use this as the member
 * variable for a class whose method returns a promise.
 */
template<typename PromiseType, typename ImplType>
class MediaPromiseHolderBase {
public:
    MediaPromiseHolderBase() = default;
    MediaPromiseHolderBase(MediaPromiseHolderBase&& other) = default;
    MediaPromiseHolderBase& operator=(MediaPromiseHolderBase&& other) = default;

    virtual ~MediaPromiseHolderBase() { ASSERT(!m_promise); }

    Ref<PromiseType> ensure(const char* methodName)
    {
        static_cast<ImplType*>(this)->check();
        if (!m_promise)
            m_promise = adoptRef(new (typename PromiseType::Private)(methodName));
        RefPtr p = m_promise;
        return p.releaseNonNull();
    }

    bool isEmpty() const
    {
        static_cast<const ImplType*>(this)->check();
        return !m_promise;
    }

    RefPtr<typename PromiseType::Private> steal()
    {
        static_cast<ImplType*>(this)->check();
        return std::exchange(m_promise, { });
    }

    template<typename ResolveValueType_>
    void resolve(ResolveValueType_&& resolveValue, const char* methodName)
    {
        static_assert(std::is_convertible_v<ResolveValueType_, typename PromiseType::ResolveValueType>, "resolve() argument must be implicitly convertible to MediaPromise's ResolveValueT");

        static_cast<ImplType*>(this)->check();
        ASSERT(m_promise);
        m_promise->resolve(std::forward<ResolveValueType_>(resolveValue), methodName);
        m_promise = nullptr;
    }

    template<typename ResolveValueType_>
    void resolveIfExists(ResolveValueType_&& resolveValue, const char* methodName)
    {
        if (!isEmpty())
            resolve(std::forward<ResolveValueType_>(resolveValue), methodName);
    }

    template<typename RejectValueType_>
    void reject(RejectValueType_&& rejectValue, const char* methodName)
    {
        static_assert(std::is_convertible_v<RejectValueType_, typename PromiseType::RejectValueType>, "reject() argument must be implicitly convertible to MediaPromise's RejectValueT");

        static_cast<ImplType*>(this)->check();
        ASSERT(m_promise);
        m_promise->reject(std::forward<RejectValueType_>(rejectValue), methodName);
        m_promise = nullptr;
    }

    template<typename RejectValueType_>
    void rejectIfExists(RejectValueType_&& rejectValue, const char* methodName)
    {
        if (!isEmpty())
            reject(std::forward<RejectValueType_>(rejectValue), methodName);
    }

    template<typename ResolveOrRejectValueType_>
    void resolveOrReject(ResolveOrRejectValueType_&& value, const char* methodName)
    {
        static_cast<ImplType*>(this)->check();
        ASSERT(m_promise);
        m_promise->resolveOrReject(std::forward<ResolveOrRejectValueType_>(value), methodName);
        m_promise = nullptr;
    }

    template<typename ResolveOrRejectValueType_>
    void resolveOrRejectIfExists(ResolveOrRejectValueType_&& value, const char* methodName)
    {
        if (!isEmpty())
            resolveOrReject(std::forward<ResolveOrRejectValueType_>(value), methodName);
    }

    void useSynchronousFunctionDispatch(const char* site)
    {
        ASSERT(m_promise);
        m_promise->useSynchronousFunctionDispatch(site);
    }

private:
    RefPtr<typename PromiseType::Private> m_promise;
};

template<typename PromiseType>
class MediaPromiseHolder
    : public MediaPromiseHolderBase<PromiseType, MediaPromiseHolder<PromiseType>> {
public:
    using MediaPromiseHolderBase<PromiseType, MediaPromiseHolder<PromiseType>>::MediaPromiseHolderBase;
    static constexpr void check() { };
};

template<typename PromiseType>
class LockedPromiseHolder : public MediaPromiseHolderBase<PromiseType, LockedPromiseHolder<PromiseType>> {
public:
    // Provide a Lock that should always be held when accessing this instance.
    explicit LockedPromiseHolder(Lock* const lock)
        : m_lock(lock)
    {
        ASSERT(lock);
    }

    LockedPromiseHolder(LockedPromiseHolder&& aOther) = delete;
    LockedPromiseHolder& operator=(LockedPromiseHolder&& aOther) = delete;

    void check() const { assertIsHeld(*m_lock); }

private:
    Lock* const m_lock;
};

/*
 * Class to encapsulate a MediaPromise::Request reference. Use this as the member
 * variable for a class waiting on a MediaPromise.
 * MediaPromiseRequestHolder is not thread-safe.
 */
template<typename PromiseType>
class MediaPromiseRequestHolder {
public:
    MediaPromiseRequestHolder() = default;
    MediaPromiseRequestHolder(MediaPromiseRequestHolder&& other) = default;
    MediaPromiseRequestHolder& operator=(MediaPromiseRequestHolder&& other) = default;
    ~MediaPromiseRequestHolder() { ASSERT(!m_request); }

    void track(RefPtr<typename PromiseType::Request> request)
    {
        ASSERT(!exists());
        m_request = WTFMove(request);
    }

    void complete()
    {
        ASSERT(exists());
        m_request = nullptr;
    }

    // Disconnects and forgets an outstanding promise. The resolve/reject methods
    // will never be called.
    void disconnect()
    {
        ASSERT(exists());
        m_request->disconnect();
        m_request = nullptr;
    }

    void disconnectIfexists()
    {
        if (exists())
            disconnect();
    }

    bool exists() const { return !!m_request; }

private:
    RefPtr<typename PromiseType::Request> m_request;
};

// Asynchronous Potentially-Cross-Thread Method Calls.
//
// This machinery allows callers to schedule a promise-returning function
// (a method and object, or a function object like a lambda) to be invoked
// asynchronously on a given thread, while at the same time receiving a
// promise upon which to invoke then() immediately. invokeAsync dispatches a
// task to invoke the function on the proper thread and also chain the
// resulting promise to the one that the caller received, so that resolve/
// reject values are forwarded through.

namespace detail {

template<typename PromiseType, typename MethodType, typename ThisType, typename... Storages>
class MethodCall {
public:
    template<typename... Args>
    MethodCall(MethodType method, ThisType* thisVal, Args&&... args)
        : m_method(method)
        , m_thisVal(thisVal)
        , m_arguments(std::forward<Args>(args)...)
    {
        static_assert(sizeof...(Storages) == sizeof...(Args), "Storages and Args should have equal sizes");
    }

    RefPtr<PromiseType> invoke() { return m_arguments.apply(m_thisVal.get(), m_method); }

private:
    MethodType m_method;
    RefPtr<ThisType> m_thisVal;

    RunnableMethodArguments<Storages...> m_arguments;
};

template<typename PromiseType, typename MethodType, typename ThisType, typename... Storages>
class ProxyRunnable {
public:
    ProxyRunnable(typename PromiseType::Private& proxyPromise, MethodCall<PromiseType, MethodType, ThisType, Storages...>* methodCall)
        : m_proxyPromise(proxyPromise)
        , m_methodCall(methodCall)
    {
    }

    void run()
    {
        RefPtr<PromiseType> p = m_methodCall->invoke();
        m_methodCall = nullptr;
        p->chainTo(WTFMove(m_proxyPromise), "<Proxy Promise>");
    }

private:
    Ref<typename PromiseType::Private> m_proxyPromise;
    std::unique_ptr<MethodCall<PromiseType, MethodType, ThisType, Storages...>> m_methodCall;
};

template<typename... Storages, typename PromiseType, typename ThisType, typename... ArgTypes, typename... ActualArgTypes>
static Ref<PromiseType> invokeAsyncImpl(SerialFunctionDispatcher& target, ThisType* thisVal, const char* callerName, Ref<PromiseType> (ThisType::*method)(ArgTypes...), ActualArgTypes&&... args)
{
    using MethodType = Ref<PromiseType> (ThisType::*)(ArgTypes...);
    using MethodCallType = detail::MethodCall<PromiseType, MethodType, ThisType, Storages...>;
    using ProxyRunnableType = detail::ProxyRunnable<PromiseType, MethodType, ThisType, Storages...>;

    auto* methodCall = new MethodCallType(method, thisVal, std::forward<ActualArgTypes>(args)...);
    auto p = adoptRef(*new (typename PromiseType::Private)(callerName));
    ProxyRunnableType r { p, methodCall };
    target.dispatch([r = WTFMove(r)] () mutable { r.run(); });

    return p;
}

constexpr bool any() { return false; }

template<typename T1>
constexpr bool any(T1 a)
{
    return static_cast<bool>(a);
}

template<typename T1, typename... Ts>
constexpr bool any(T1 a, Ts... others)
{
    return a || any(others...);
}

} // namespace detail

template<typename... Storages, typename PromiseType, typename ThisType, typename... ArgTypes, typename... ActualArgTypes>
static Ref<PromiseType> invokeAsync(SerialFunctionDispatcher& target, ThisType* thisVal, const char* callerName, Ref<PromiseType> (ThisType::*method)(ArgTypes...), ActualArgTypes&&... args)
{
    if constexpr (sizeof...(Storages) != 0) {
        // invokeAsync with explicitly-specified storages. See ParameterStorage in ParametersStorage.h for help.
        static_assert(sizeof...(Storages) == sizeof...(ArgTypes), "Provided Storages and method's ArgTypes should have equal sizes");
        static_assert(sizeof...(Storages) == sizeof...(ActualArgTypes), "Provided Storages and ActualArgTypes should have equal sizes");
        return detail::invokeAsyncImpl<Storages...>(target, thisVal, callerName, method, std::forward<ActualArgTypes>(args)...);
    } else {
        // invokeAsync with no explicitly-specified storages, will copy arguments and then move them out of the runnable into the target method parameters.
        static_assert(!detail::any(std::is_pointer_v<std::remove_reference_t<ActualArgTypes>>...), "Cannot pass pointer types through invokeAsync, Storages must be provided");
        static_assert(sizeof...(ArgTypes) == sizeof...(ActualArgTypes), "Method's ArgTypes and ActualArgTypes should have equal sizes");
        return detail::invokeAsyncImpl<StoreCopyPassByRRef<std::decay_t<ActualArgTypes>>...>(target, thisVal, callerName, method, std::forward<ActualArgTypes>(args)...);
    }
}

namespace detail {

template<typename Function, typename PromiseType>
class ProxyFunctionRunnable {
    using FunctionStorage = std::decay_t<Function>;

public:
    template<typename F>
    ProxyFunctionRunnable(typename PromiseType::Private& proxyPromise, F&& function)
        : m_proxyPromise(proxyPromise)
        , m_function(new FunctionStorage(std::forward<F>(function)))
    {
    }

    void run()
    {
        RefPtr<PromiseType> p = (*m_function)();
        m_function = nullptr;
        p->chainTo(WTFMove(m_proxyPromise), "<Proxy Promise>");
    }

private:
    Ref<typename PromiseType::Private> m_proxyPromise;
    std::unique_ptr<FunctionStorage> m_function;
};

// Note: The following struct and function are not for public consumption (yet?)
// as we would prefer all calls to pass on-the-spot lambdas (or at least moved
// function objects). They could be moved outside of detail if really needed.

// We prefer getting function objects by non-lvalue-ref (to avoid copying them
// and their captures). This struct is a tag that allows the use of objects
// through lvalue-refs where necessary.
struct AllowinvokeAsyncFunctionLVRef { };

// Invoke a function object (e.g., lambda or std/wtf::function) asynchronously; note that the object will be copied if provided by
// lvalue-ref. Return a promise that the function should eventually resolve or reject.
template<typename Function>
static auto invokeAsync(SerialFunctionDispatcher& target, const char* callerName, AllowinvokeAsyncFunctionLVRef, Function&& function) -> decltype(function())
{
    static_assert(IsSmartRef<decltype(function())>::value && IsMediaPromise<typename RemoveSmartPointer<decltype(function())>::type>::value, "Function object must return Ref<MediaPromise>");
    using PromiseType = typename RemoveSmartPointer<decltype(function())>::type;
    using ProxyRunnableType = detail::ProxyFunctionRunnable<Function, PromiseType>;

    auto p = adoptRef(*new PromiseType::Private(callerName));
    ProxyRunnableType r { p.get(), std::forward<Function>(function) };
    target.dispatch([r = WTFMove(r)] () mutable { r.run(); });
    return p;
}

} // namespace detail

// Invoke a function object (e.g., lambda) asynchronously.
// Return a promise that the function should eventually resolve or reject.
template<typename Function>
static auto invokeAsync(SerialFunctionDispatcher& target, const char* callerName, Function&& function) -> decltype(function())
{
    static_assert(!std::is_lvalue_reference_v<Function>, "Function object must not be passed by lvalue-ref (to avoid unplanned copies); Consider move()ing the object.");
    return detail::invokeAsync(target, callerName, detail::AllowinvokeAsyncFunctionLVRef(), std::forward<Function>(function));
}

} // namespace WebCore
