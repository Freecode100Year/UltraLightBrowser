#pragma once
namespace wil {
template<class T> class com_ptr {
    T* value = nullptr;
public:
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    T** operator&() { return &value; }
};
}
