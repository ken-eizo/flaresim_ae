#pragma once
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
using FlareLensNativeWindow = HWND;
#else
using FlareLensNativeWindow = void*;
#endif
namespace fl::catalog {
// User-triggered native modal lens browser; never called on AE render threads.
bool choose_embedded_lens(FlareLensNativeWindow owner,int current_index,int& chosen_index);
}
