#pragma once
#ifdef _WIN32
#include <windows.h>
// No I/O or locks on the Windows control-handler thread.
inline BOOL WINAPI ignore_copy_interrupt(DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}
#endif
