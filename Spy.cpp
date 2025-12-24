// ---------------------------------------------------------
// [Tool] Explorer Event Spy (Fixed)
// [Purpose] Analyze WinEvent sequence for Explorer Close/Minimize
// ---------------------------------------------------------

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <iostream>
#include <string>
#include <iomanip>

// MinGW 등에서 누락된 상수 수동 정의
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif

#ifndef EVENT_OBJECT_UNCLOAKED
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

DWORD g_startTime = 0;

std::wstring EventToString(DWORD event) {
    switch (event) {
    case EVENT_OBJECT_CREATE:         return L"CREATE";
    case EVENT_OBJECT_DESTROY:        return L"DESTROY (DEAD)";
    case EVENT_OBJECT_SHOW:           return L"SHOW";
    case EVENT_OBJECT_HIDE:           return L"HIDE";
    case EVENT_OBJECT_FOCUS:          return L"FOCUS";
    case EVENT_SYSTEM_FOREGROUND:     return L"FOREGROUND (Active Change)";
    case EVENT_SYSTEM_MINIMIZESTART:  return L"MINIMIZE_START";
    case EVENT_SYSTEM_MINIMIZEEND:    return L"MINIMIZE_END";
    case EVENT_OBJECT_CLOAKED:        return L"CLOAKED (Covered by DWM)";
    case EVENT_OBJECT_UNCLOAKED:      return L"UNCLOAKED";
    case EVENT_OBJECT_NAMECHANGE:     return L"NAME_CHANGE";
    default: return L"UNKNOWN_EVENT (" + std::to_wstring(event) + L")";
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, 
                           LONG idObject, LONG idChild, 
                           DWORD dwEventThread, DWORD dwmsEventTime) {
    
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!IsWindow(hwnd)) return; 

    wchar_t className[256];
    if (GetClassNameW(hwnd, className, 256) == 0) return;
    
    // 탐색기(CabinetWClass)만 감시
    if (wcscmp(className, L"CabinetWClass") != 0) return;

    // 위치 변경은 로그가 너무 많아서 제외
    if (event == EVENT_OBJECT_LOCATIONCHANGE) return; 

    DWORD currentTime = GetTickCount();
    DWORD elapsed = currentTime - g_startTime;

    std::wcout << L"[" << std::setw(6) << elapsed << L"ms] " 
               << L"HWND: " << std::hex << hwnd << std::dec << L" | " 
               << EventToString(event) << std::endl;
}

int main() {
    g_startTime = GetTickCount();

    std::wcout << L"=== Explorer Event Spy Started ===" << std::endl;
    std::wcout << L"=== Open Explorer -> Minimize -> Close it ===" << std::endl;

    HWINEVENTHOOK hHook = SetWinEventHook(
        EVENT_MIN, EVENT_MAX, 
        NULL, WinEventProc, 
        0, 0, 
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (!hHook) {
        std::wcout << L"Hook Failed!" << std::endl;
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWinEvent(hHook);
    return 0;
}