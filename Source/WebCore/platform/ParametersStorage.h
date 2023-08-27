/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 * Copyright (C) 2015-2019 Mozilla Foundation. All rights reserved.
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

#include "TypeTraits.h"

// IsParameterStorageClass<T>::value is true if T is a parameter-storage class
// that will be recognized by RunnableMethodArguments to force a specific
// storage&passing strategy (instead of inferring one, see ParameterStorage below).
// When creating a new storage class, add a specialization for it to be recognized.
template<typename T>
struct IsParameterStorageClass : public std::false_type { };

// StoreXPassByY structs used to inform nsRunnableMethodArguments how to
// store arguments, and how to pass them to the target method.

template<typename T>
struct StoreCopyPassByValue {
    using stored_type = std::decay_t<T>;
    typedef stored_type passed_type;
    stored_type m;
    template<typename A>
    StoreCopyPassByValue(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return m; }
};
template<typename S>
struct IsParameterStorageClass<StoreCopyPassByValue<S>> : public std::true_type { };

template<typename T>
struct StoreCopyPassByConstLRef {
    using stored_type = std::decay_t<T>;
    typedef const stored_type& passed_type;
    stored_type m;
    template<typename A>
    StoreCopyPassByConstLRef(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return m; }
};
template<typename S>
struct IsParameterStorageClass<StoreCopyPassByConstLRef<S>> : public std::true_type { };

template<typename T>
struct StoreCopyPassByLRef {
    using stored_type = std::decay_t<T>;
    typedef stored_type& passed_type;
    stored_type m;
    template<typename A>
    StoreCopyPassByLRef(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return m; }
};
template<typename S>
struct IsParameterStorageClass<StoreCopyPassByLRef<S>> : public std::true_type { };

template<typename T>
struct StoreCopyPassByRRef {
    using stored_type = std::decay_t<T>;
    typedef stored_type&& passed_type;
    stored_type m;
    template<typename A>
    StoreCopyPassByRRef(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return WTFMove(m); }
};
template<typename S>
struct IsParameterStorageClass<StoreCopyPassByRRef<S>> : public std::true_type { };

template<typename T>
struct StoreRefPassByLRef {
    typedef T& stored_type;
    typedef T& passed_type;
    stored_type m;
    template<typename A>
    StoreRefPassByLRef(A& a)
        : m(a)
    {
    }
    passed_type passAsParameter() { return m; }
};
template<typename S>
struct IsParameterStorageClass<StoreRefPassByLRef<S>> : public std::true_type { };

template<typename T>
struct StoreConstRefPassByConstLRef {
    typedef const T& stored_type;
    typedef const T& passed_type;
    stored_type m;
    template<typename A>
    StoreConstRefPassByConstLRef(const A& a)
        : m(a)
    {
    }
    passed_type passAsParameter() { return m; }
};

template<typename S>
struct IsParameterStorageClass<StoreConstRefPassByConstLRef<S>> : public std::true_type { };

template<typename T>
struct StoreRefPtrPassByPtr {
    typedef RefPtr<T> stored_type;
    typedef T* passed_type;
    stored_type m;
    template<typename A>
    StoreRefPtrPassByPtr(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return m.get(); }
};
template<typename S>
struct IsParameterStorageClass<StoreRefPtrPassByPtr<S>> : public std::true_type { };

template<typename T>
struct StoreRefPassLRef {
    typedef Ref<T> stored_type;
    typedef T& passed_type;
    stored_type m;
    template<typename A>
    StoreRefPassLRef(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return m.get(); }
};
template<typename S>
struct IsParameterStorageClass<StoreRefPassLRef<S>> : public std::true_type { };

template<typename T>
struct StorePtrPassByPtr {
    typedef T* stored_type;
    typedef T* passed_type;
    stored_type m;
    template<typename A>
    StorePtrPassByPtr(A a)
        : m(a)
    {
    }
    passed_type passAsParameter() { return m; }
};
template<typename S>
struct IsParameterStorageClass<StorePtrPassByPtr<S>> : public std::true_type { };

template<typename T>
struct StoreConstPtrPassByConstPtr {
    typedef const T* stored_type;
    typedef const T* passed_type;
    stored_type m;
    template<typename A>
    StoreConstPtrPassByConstPtr(A a)
        : m(a)
    {
    }
    passed_type passAsParameter() { return m; }
};
template<typename S>
struct IsParameterStorageClass<StoreConstPtrPassByConstPtr<S>> : public std::true_type { };

template<typename T>
struct StoreCopyPassByConstPtr {
    typedef T stored_type;
    typedef const T* passed_type;
    stored_type m;
    template<typename A>
    StoreCopyPassByConstPtr(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return &m; }
};
template<typename S>
struct IsParameterStorageClass<StoreCopyPassByConstPtr<S>> : public std::true_type { };

template<typename T>
struct StoreCopyPassByPtr {
    typedef T stored_type;
    typedef T* passed_type;
    stored_type m;
    template<typename A>
    StoreCopyPassByPtr(A&& a)
        : m(std::forward<A>(a))
    {
    }
    passed_type passAsParameter() { return &m; }
};
template<typename S>
struct IsParameterStorageClass<StoreCopyPassByPtr<S>> : public std::true_type { };

template<typename TWithoutPointer>
struct NonSmartPointerStorageClass
    : std::conditional<std::is_const_v<TWithoutPointer>, StoreConstPtrPassByConstPtr<std::remove_const_t<TWithoutPointer>>, StorePtrPassByPtr<TWithoutPointer>> {
    using type = typename NonSmartPointerStorageClass::conditional::type;
};

template<typename TWithoutPointer>
struct PointerStorageClass
    : std::conditional<HasRefCountMethods<TWithoutPointer>::value, StoreRefPtrPassByPtr<TWithoutPointer>, typename NonSmartPointerStorageClass<TWithoutPointer>::type> {
    using type = typename PointerStorageClass::conditional::type;
};

template<typename TWithoutRef>
struct LValueReferenceStorageClass
    : std::conditional<std::is_const_v<TWithoutRef>, StoreConstRefPassByConstLRef<std::remove_const_t<TWithoutRef>>, StoreRefPassByLRef<TWithoutRef>> {
    using type = typename LValueReferenceStorageClass::conditional::type;
};

template<typename T>
struct SmartPointerStorageClass
    : std::conditional<IsRefcountedSmartPointer<T>::value, StoreRefPtrPassByPtr<typename RemoveSmartPointer<T>::type>, StoreCopyPassByConstLRef<T>> {
    using type = typename SmartPointerStorageClass::conditional::type;
};

template<typename T>
struct SmartRefStorageClass
    : std::conditional<IsSmartRef<T>::value, StoreRefPassByLRef<typename RemoveSmartPointer<T>::type>, SmartPointerStorageClass<T>> {
    using type = typename SmartRefStorageClass::conditional::type;
};

template<typename T>
struct NonLValueReferenceStorageClass
    : std::conditional<std::is_rvalue_reference_v<T>, StoreCopyPassByRRef<std::remove_reference_t<T>>, typename SmartRefStorageClass<T>::type> {
    using type = typename NonLValueReferenceStorageClass::conditional::type;
};

template<typename T>
struct NonPointerStorageClass
    : std::conditional<std::is_lvalue_reference_v<T>, typename LValueReferenceStorageClass<std::remove_reference_t<T>>::type, typename NonLValueReferenceStorageClass<T>::type> {
    using type = typename NonPointerStorageClass::conditional::type;
};

template<typename T>
struct NonParameterStorageClass
    : std::conditional<std::is_pointer_v<T>, typename PointerStorageClass<std::remove_pointer_t<T>>::type, typename NonPointerStorageClass<T>::type> {
    using type = typename NonParameterStorageClass::conditional::type;
};

// Choose storage&passing strategy based on preferred storage type:
// - If IsParameterStorageClass<T>::value is true, use as-is.
// - RC*       -> StoreRefPtrPassByPtr<RC>       :Store RefPtr<RC>, pass RC*
// - RC&       -> StoreRefPassLRef<RC>           :Store Ref<RC>, pass RC&
//   ^^ RC quacks like a ref-counted type (i.e., has ref() and deref() methods)
// - const T*  -> StoreConstPtrPassByConstPtr<T> :Store const T*, pass const T*
// - T*        -> StorePtrPassByPtr<T>           :Store T*, pass T*.
// - const T&  -> StoreConstRefPassByConstLRef<T>:Store const T&, pass const T&.
// - T&        -> StoreRefPassByLRef<T>          :Store T&, pass T&.
// - T&&       -> StoreCopyPassByRRef<T>         :Store T, pass WTFMove(T).
// - Other T   -> StoreCopyPassByConstLRef<T>    :Store T, pass const T&.
// Other available explicit options:
// -              StoreCopyPassByValue<T>        :Store T, pass T.
// -              StoreCopyPassByLRef<T>         :Store T, pass T& (of copy!)
// -              StoreCopyPassByConstPtr<T>     :Store T, pass const T*
// -              StoreCopyPassByPtr<T>          :Store T, pass T* (of copy!)
// Or create your own class with passAsParameter() method, optional
// clean-up in destructor, and with associated IsParameterStorageClass<>.
template<typename T>
struct ParameterStorage
    : std::conditional<IsParameterStorageClass<T>::value, T, typename NonParameterStorageClass<T>::type> {
    using type = typename ParameterStorage::conditional::type;
};

// struct used to store arguments and later apply them to a method.
template<typename... Ts>
struct RunnableMethodArguments final {
    std::tuple<typename ParameterStorage<Ts>::type...> arguments;
    template<typename... As>
    explicit RunnableMethodArguments(As&&... arguments) : arguments(std::forward<As>(arguments)...) { }
    template<class C, typename M>
    decltype(auto) apply(C* o, M m)
    {
        return std::apply([&o, m](auto&&... args) {
            return ((*o).*m)(args.passAsParameter()...);
        }, arguments);
    }
};
