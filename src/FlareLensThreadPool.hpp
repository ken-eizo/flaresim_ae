#pragma once
#include <cstddef>
#include <functional>
#include <vector>
namespace fl {
// One persistent worker pool shared by every render thread (AE MFR).
// parallel_for runs body(i) for every i in [0,count) on the workers AND the
// calling thread, and returns when all indices have finished. Concurrent
// callers share the workers; a body may itself call parallel_for.
// Exceptions thrown by body are rethrown on the calling thread.
// FLARESIM_THREADS=<n> caps the total thread count (1 = serial).
int thread_count() noexcept;
void parallel_for(int count,const std::function<void(int)>& body);
// Joins the workers. Call from PF_Cmd_GLOBAL_SETDOWN, never from DllMain.
void shutdown_thread_pool() noexcept;

// Full-frame float buffers recycled across frames, so a 1080p/4K image is
// not page-faulted and zeroed by the OS on every render. A leased buffer has
// size n and unspecified contents. Thread-safe; bounded total size.
std::vector<float> acquire_buffer(size_t n);
void release_buffer(std::vector<float>&& buffer) noexcept;
// Parallel fill, faster than assign() for full-frame images.
void parallel_zero(std::vector<float>& buffer);
}
