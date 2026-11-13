#include <windows.h>
extern "C" {
__declspec(dllexport) void WinDivertOpen() {}
__declspec(dllexport) void WinDivertRecv() {}
__declspec(dllexport) void WinDivertShutdown() {}
__declspec(dllexport) void WinDivertClose() {}
__declspec(dllexport) void WinDivertSend() {}
}

