#pragma once
#include <cstddef>
namespace fl::catalog::embedded {
struct EmbeddedLens {const char* data=nullptr;std::size_t size=0;};
EmbeddedLens get(int zero_based) noexcept;
}
