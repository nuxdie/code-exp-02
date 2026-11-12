#pragma once

#include <memory>

// Owning handles for the C libraries. Each one releases with the library's own function,
// so nothing that holds one needs a destructor.
template <auto release> struct Releaser {
    template <class Object> void operator()(Object* object) const { release(object); }
};
template <class Object, auto release> using Handle = std::unique_ptr<Object, Releaser<release>>;
