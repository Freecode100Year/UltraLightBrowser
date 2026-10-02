#pragma once
#include <utility>
namespace Microsoft::WRL {
template<class T, class F> struct CallbackHolder : T {
    F fn;
    explicit CallbackHolder(F f) : fn(std::move(f)) {}
    HRESULT Invoke(HRESULT hr, BOOL successful) override { return fn(hr, successful); }
    T* Get() { return this; }
};
template<class T, class F> CallbackHolder<T, F> Callback(F f) { return CallbackHolder<T, F>(std::move(f)); }
}
