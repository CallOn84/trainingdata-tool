#pragma once

namespace absl {
template <typename T>
struct Cleanup {
    Cleanup(T) {}
    ~Cleanup() {} // No-op for now, assuming NDEBUG or unused
};
}
