#pragma once
#define NOMINMAX
#include <windows.h>
namespace fl::catalog {
// Only the Adobe user-changed-parameter selector calls this modal browser.
// No After Effects GUI operations are initiated by the development agent.
bool choose_embedded_lens(HWND owner,int current_index,int& chosen_index);
}
