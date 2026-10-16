#pragma once
#include "lens.h"
#include "FlareLensCatalogGenerated.hpp"
#include <cstddef>
#include <cstdint>

namespace fl::catalog {
// CMake keeps this manifest lexically sorted. A 1-based lens index is
// stable for a given upstream repository version and is saved in AE projects.
const LensSystem* lens_by_index(int one_based) noexcept;
int index_for_filename(const char* filename) noexcept;
int quick_preset_index(int one_based_preset) noexcept;
const char* filename(int one_based) noexcept;

// A POD-only diagnostic export: test the exact AEX embedded resources without
// needing to launch AE. No hidden fallback to any other lens is permissible.
struct Report {
 std::uint32_t size=sizeof(Report);
 int count=0;
 int index=0;
 int valid=0;
 int renderable=0; // needs >=2 surfaces for a ghost bounce pair
 int surface_count=0;
 float focal_length=0, sensor_z=0;
 std::uint64_t geometry_hash=0;
 char filename[256]{};
};
std::uint64_t geometry_hash(const LensSystem& lens) noexcept;
}
#if defined(_WIN32)
#define FLARELENS_EXPORT __declspec(dllexport)
#else
#define FLARELENS_EXPORT __attribute__((visibility("default")))
#endif
extern "C" FLARELENS_EXPORT
int FlareLensInspectCatalog(int one_based,fl::catalog::Report* out);
extern "C" FLARELENS_EXPORT
int FlareLensQuickPresetCatalogID(int quick_preset);
