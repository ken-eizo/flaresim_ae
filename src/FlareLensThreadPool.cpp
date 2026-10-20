#include "FlareLensThreadPool.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace fl {
namespace {
struct Job {
 const std::function<void(int)>* body=nullptr;
 int count=0;
 std::atomic<int> next{0},done{0};
 int helpers=0; // workers inside run(); guarded by Pool::mutex
 std::exception_ptr error;
 std::mutex error_mutex;
};

class Pool {
public:
 int size() noexcept {start();return int(workers.size())+1;}
 void run(int count,const std::function<void(int)>& body){
  if(count<=0)return;
  start();
  if(count==1||workers.empty()){
   for(int i=0;i<count;i++)body(i);
   return;
  }
  Job job;
  job.body=&body;job.count=count;
  {
   std::lock_guard<std::mutex> lock(mutex);
   queue.push_back(&job);
  }
  wake.notify_all();
  work(job);
  std::unique_lock<std::mutex> lock(mutex);
  // No worker may join once the job leaves the queue; wait for the ones
  // still inside it before the stack-allocated job goes out of scope.
  const auto it=std::find(queue.begin(),queue.end(),&job);
  if(it!=queue.end())queue.erase(it);
  finished.wait(lock,[&]{
   return job.done.load(std::memory_order_acquire)==job.count&&job.helpers==0;
  });
  lock.unlock();
  if(job.error)std::rethrow_exception(job.error);
 }
 void shutdown() noexcept {
  std::vector<std::thread> joining;
  {
   std::lock_guard<std::mutex> lock(mutex);
   stop=true;
   joining.swap(workers);
  }
  wake.notify_all();
  for(auto& t:joining)if(t.joinable())t.join();
  std::lock_guard<std::mutex> lock(mutex);
  stop=false;started=false;
 }
private:
 void start(){
  std::lock_guard<std::mutex> lock(mutex);
  if(started)return;
  started=true;
  int total=int(std::max(1u,std::thread::hardware_concurrency()));
  if(const char* env=std::getenv("FLARESIM_THREADS")){
   const int requested=std::atoi(env);
   if(requested>0)total=std::min(total,requested);
  }
  total=std::clamp(total,1,64);
  for(int i=1;i<total;i++)workers.emplace_back([this]{loop();});
 }
 void work(Job& job){
  for(;;){
   const int i=job.next.fetch_add(1,std::memory_order_relaxed);
   if(i>=job.count)return;
   try{(*job.body)(i);}
   catch(...){
    std::lock_guard<std::mutex> lock(job.error_mutex);
    if(!job.error)job.error=std::current_exception();
   }
   if(job.done.fetch_add(1,std::memory_order_acq_rel)+1==job.count){
    std::lock_guard<std::mutex> lock(mutex);
    finished.notify_all();
   }
  }
 }
 void loop(){
  std::unique_lock<std::mutex> lock(mutex);
  for(;;){
   wake.wait(lock,[&]{return stop||!queue.empty();});
   if(stop)return;
   Job* job=queue.front();
   if(job->next.load(std::memory_order_relaxed)>=job->count){
    queue.pop_front();
    continue;
   }
   job->helpers++;
   lock.unlock();
   work(*job);
   lock.lock();
   job->helpers--;
   if(!queue.empty()&&queue.front()==job)queue.pop_front();
   finished.notify_all();
  }
 }
 std::mutex mutex;
 std::condition_variable wake,finished;
 std::deque<Job*> queue;
 std::vector<std::thread> workers;
 bool started=false,stop=false;
};

Pool& pool(){
 // Deliberately never destroyed: joining workers from a static destructor
 // can deadlock on the loader lock at DLL unload. GLOBAL_SETDOWN joins them.
 static Pool* instance=new Pool;
 return *instance;
}
} // namespace

int thread_count() noexcept {return pool().size();}
void parallel_for(int count,const std::function<void(int)>& body){pool().run(count,body);}

namespace {
struct BufferCache {
 std::mutex mutex;
 std::vector<std::vector<float>> free;
 size_t bytes=0;
};
BufferCache& buffers(){
 static BufferCache* cache=new BufferCache;
 return *cache;
}
constexpr size_t kCacheBytes=size_t(512)<<20;
constexpr size_t kCacheCount=12;
} // namespace

std::vector<float> acquire_buffer(size_t n){
 auto& cache=buffers();
 std::vector<float> result;
 {
  std::lock_guard<std::mutex> lock(cache.mutex);
  // Smallest cached buffer that is large enough.
  size_t best=cache.free.size();
  for(size_t i=0;i<cache.free.size();i++){
   if(cache.free[i].capacity()<n)continue;
   if(best==cache.free.size()||cache.free[i].capacity()<cache.free[best].capacity())best=i;
  }
  if(best<cache.free.size()){
   result.swap(cache.free[best]);
   cache.bytes-=result.capacity()*sizeof(float);
   cache.free.erase(cache.free.begin()+std::ptrdiff_t(best));
  }
 }
 if(result.capacity()>=n){
  // Shrinking or growing within capacity never reallocates; growing
  // value-initializes only the new tail.
  result.resize(n);
  return result;
 }
 result.clear();
 result.shrink_to_fit();
 result.resize(n);
 return result;
}

void release_buffer(std::vector<float>&& buffer) noexcept {
 if(buffer.capacity()<(size_t(1)<<16))return; // small buffers are cheap
 auto& cache=buffers();
 std::vector<float> dropped;
 try{
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.bytes+=buffer.capacity()*sizeof(float);
  cache.free.push_back(std::move(buffer));
  // Over budget: evict the oldest entries first.
  while(!cache.free.empty()&&(cache.bytes>kCacheBytes||cache.free.size()>kCacheCount)){
   cache.bytes-=cache.free.front().capacity()*sizeof(float);
   dropped.swap(cache.free.front());
   cache.free.erase(cache.free.begin());
   dropped=std::vector<float>();
  }
 }catch(...){}
}

void parallel_zero(std::vector<float>& buffer){
 constexpr size_t kBlock=size_t(1)<<16;
 const size_t blocks=(buffer.size()+kBlock-1)/kBlock;
 float* data=buffer.data();
 const size_t n=buffer.size();
 parallel_for(int(blocks),[&](int b){
  const size_t begin=size_t(b)*kBlock,end=std::min(n,begin+kBlock);
  std::fill(data+begin,data+end,0.f);
 });
}

void shutdown_thread_pool() noexcept {
 pool().shutdown();
 auto& cache=buffers();
 std::lock_guard<std::mutex> lock(cache.mutex);
 cache.free.clear();
 cache.free.shrink_to_fit();
 cache.bytes=0;
}
} // namespace fl
