#include "FlareLensCpuParallel.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <cmath>

namespace fl {
namespace {
struct Group {
 std::future<Image> future;
};
int cancelled(void* ptr){
 return static_cast<std::atomic<int>*>(ptr)->load(std::memory_order_relaxed);
}
}
Image render_sources_parallel(const LensSystem& lens,
 const std::vector<Source>& sources,const Settings& settings,
 int width,int height,RenderControl control,int requested_threads){
 // AE cancellation belongs on the render thread; honour an already
 // cancelled frame before launching any asynchronous optical tasks.
 if(control.check_abort){
  if(const int code=control.check_abort(control.context)){
   Image aborted{};aborted.width=width;aborted.height=height;
   aborted.abort_error=code;return aborted;
  }
 }
 if(sources.size()<4||width<1||height<1||requested_threads<=1)
  return render_cpu(lens,sources,settings,width,height,control);
 // Full float frame per worker is 12 bytes/px. A strict 256 MiB budget
 // prevents a single huge MFR frame from multiplying RAM without limit.
 const size_t bytes_per_frame=size_t(width)*height*3*sizeof(float);
 if(bytes_per_frame>size_t(96)*1024*1024)
  return render_cpu(lens,sources,settings,width,height,control);
 const unsigned available=std::max(1u,std::thread::hardware_concurrency());
 const int max_workers=std::min<int>({
  requested_threads,4,int(sources.size()/4),int(available),
  std::max<int>(1,int(size_t(256)*1024*1024/
       std::max<size_t>(1,bytes_per_frame)))});
 if(max_workers<=1)return render_cpu(lens,sources,settings,width,height,control);
 Image total{width,height,{}};
 std::atomic<int> abort_flag{0};
 std::vector<Group> workers;
 workers.reserve(max_workers);
 for(int t=0;t<max_workers;t++){
  const size_t start=sources.size()*size_t(t)/max_workers;
  const size_t end=sources.size()*size_t(t+1)/max_workers;
  std::vector<Source> batch(sources.begin()+start,sources.begin()+end);
  workers.push_back({std::async(std::launch::async,
   [&lens,settings,width,height,&abort_flag,group=std::move(batch)]()mutable{
    return render_cpu(lens,group,settings,width,height,
      {&cancelled,&abort_flag});
   })});
 }
 // The AE render thread, not an arbitrary worker, calls the host callback.
 // This also makes CPU fallback cancelable during >100-second renders.
 bool unfinished=true;
 while(unfinished){
  unfinished=false;
  for(auto& worker:workers){
   if(worker.future.wait_for(std::chrono::milliseconds(0))!=
       std::future_status::ready){unfinished=true;break;}
  }
  if(unfinished){
   if(control.check_abort){
    if(const int e=control.check_abort(control.context))
     abort_flag.store(e,std::memory_order_relaxed);
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
 }
 if(abort_flag.load(std::memory_order_relaxed)!=0){
  total.abort_error=abort_flag.load(std::memory_order_relaxed);
  for(auto& worker:workers)worker.future.get();
  return total;
 }
 total.rgb.assign(size_t(width)*height*3,0.f);
 for(auto& worker:workers){
  auto layer=worker.future.get();
  if(layer.abort_error){total.abort_error=layer.abort_error;return total;}
  if(layer.rgb.size()!=total.rgb.size()){total.abort_error=1;return total;}
  for(size_t i=0;i<total.rgb.size();i++)total.rgb[i]+=layer.rgb[i];
  total.diagnostics.active_pairs+=layer.diagnostics.active_pairs;
  total.diagnostics.traced_rays+=layer.diagnostics.traced_rays;
  total.diagnostics.valid_rays+=layer.diagnostics.valid_rays;
  total.diagnostics.sensor_hits+=layer.diagnostics.sensor_hits;
 }
 // Per-worker diagnostics count lit pixels multiple times. Recompute once
 // after the deterministic contiguous-batch sum.
 total.diagnostics.screen_samples=0;
 total.diagnostics.max_luminance=0.f;
 total.diagnostics.sum_luminance=0.;
 for(size_t i=0;i<total.rgb.size();i+=3){
  const float l=.2126f*total.rgb[i]+
                .7152f*total.rgb[i+1]+.0722f*total.rgb[i+2];
  if(l>0&&std::isfinite(l)){
   total.diagnostics.screen_samples++;
   total.diagnostics.max_luminance=
    std::max(total.diagnostics.max_luminance,l);
   total.diagnostics.sum_luminance+=l;
  }
 }
 return total;
}
} // namespace fl
