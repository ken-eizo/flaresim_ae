#pragma once
#include "FlareLensCore.hpp"
namespace fl {
// CPU-only fallback, used when the host cannot expose a compatible CUDA
// World. Independent light batches use private images; no Adobe Suite is
// invoked from worker threads. The calling thread retains host cancel polling.
Image render_sources_parallel(const LensSystem&,const std::vector<Source>&,
  const Settings&,int width,int height,RenderControl control={},
  int requested_threads=3);
}
