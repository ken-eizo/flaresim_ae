#pragma once
// Development-only, bounded, nonblocking-best-effort telemetry from the REAL
// After Effects render selectors. No GUI, no AE process control, no video data.
// The tracer is opt-out by an appended AE checkbox and deliberately records
// only scalar render metadata / optical energy diagnostics.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cstring>

namespace fl {
inline void host_trace(bool enabled,const char* stage,const char* facts) noexcept{
 if(!enabled)return;
 // Interactive parameter drags may issue hundreds of speculative
 // PRE_RENDERs with no actual draw. Reserve room for real GPU_DONE lines.
 static std::atomic<unsigned> pre_render_count{0};
 if(stage&&(std::strcmp(stage,"PRE_RENDER")==0||
            std::strcmp(stage,"GPU_ELIGIBILITY")==0)){
  if(pre_render_count.fetch_add(1,std::memory_order_relaxed)>=160)return;
 }
 static std::atomic<unsigned> record_count{0};
 const unsigned id=record_count.fetch_add(1,std::memory_order_relaxed);
 if(id>=512)return;
 wchar_t temp[MAX_PATH]={};
 const DWORD n=GetTempPathW(MAX_PATH,temp);
 if(n==0||n>=MAX_PATH-64)return;
 wchar_t filename[MAX_PATH]={};
 std::swprintf(filename,MAX_PATH,L"%lsFlareSimAE_host_%lu.log",
               temp,GetCurrentProcessId());
 const HANDLE file=CreateFileW(filename,FILE_APPEND_DATA,
       FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,
       FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return;
 SYSTEMTIME now{};
 GetLocalTime(&now);
 char line[1600]={};
 const int length=std::snprintf(line,sizeof(line),
  "%04d-%02d-%02d %02d:%02d:%02d.%03d pid=%lu tid=%lu seq=%u "
  "version=1.3.8 phase=%s %s\r\n",
  int(now.wYear),int(now.wMonth),int(now.wDay),
  int(now.wHour),int(now.wMinute),int(now.wSecond),int(now.wMilliseconds),
  GetCurrentProcessId(),GetCurrentThreadId(),id,stage?stage:"-",facts?facts:"");
 if(length>0&&size_t(length)<sizeof(line)){
  DWORD written=0;
  WriteFile(file,line,DWORD(length),&written,nullptr);
 }
 CloseHandle(file);
}
}
