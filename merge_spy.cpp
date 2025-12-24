// ---------------------------------------------------------
// [Tool] Tab Merge Spy (English & Safe)
// [Purpose] Analyze Tab Merge/Split Events
// ---------------------------------------------------------

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <iostream>
#include <string>
#include <iomanip>
#include <vector>

// Define missing constants
#ifndef EVENT_OBJECT_PARENTCHANGE
#define EVENT_OBJECT_PARENTCHANGE 0x800F
#endif

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif

DWORD g_startTime = 0;

std::wstring EventToString(DWORD event) {
    switch (event) {
    case EVENT_OBJECT_CREATE:         return L"CREATE";
    case EVENT_OBJECT_DESTROY:        return L"DESTROY (DEAD)";
    case EVENT_OBJECT_SHOW:           return L"SHOW";
    case EVENT_OBJECT_HIDE:           return L"HIDE";
    case EVENT_SYSTEM_FOREGROUND:     return L"FOREGROUND (Focus Change)";
    case EVENT_OBJECT_LOCATIONCHANGE: return L"LOCATION_CHANGE"; 
    case EVENT_OBJECT_PARENTCHANGE:   return L"PARENT_CHANGE (Tab Moving!)"; // 🔥 Target
    case EVENT_OBJECT_NAMECHANGE:     return L"NAME_CHANGE";
    case EVENT_OBJECT_CLOAKED:        return L"CLOAKED";
    default: return L"UNKNOWN (" + std::to_wstring(event) + L")";
    }
}

void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, 
                           LONG idObject, LONG idChild, 
                           DWORD dwEventThread, DWORD dwmsEventTime) {
    
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    
    // Check if handle is valid NOW
    bool isAlive = IsWindow(hwnd);

    // Filter: Only Explorer (CabinetWClass) or Destroy events
    wchar_t className[256] = L"";
    if (isAlive) {
        GetClassNameW(hwnd, className, 256);
    } else {
        wcscpy_s(className, L"DEAD_WINDOW");
    }

    // Only allow CabinetWClass or events that might happen to dying windows
    if (wcscmp(className, L"CabinetWClass") != 0 && event != EVENT_OBJECT_DESTROY) return;

    // Filter out LocationChange (Too noisy during drag)
    if (event == EVENT_OBJECT_LOCATIONCHANGE) return; 

    DWORD currentTime = GetTickCount();
    DWORD elapsed = currentTime - g_startTime;

    std::wcout << L"[" << std::setw(6) << elapsed << L"ms] " 
               << L"HWND: " << std::hex << hwnd << std::dec 
               << L" | Alive: " << (isAlive ? L"O" : L"X") << L" | " 
               << EventToString(event) << std::endl;
}

int main() {
    // Remove locale settings to prevent runtime_error
    // std::wcout.imbue(std::locale("")); <--- Removed this line

    g_startTime = GetTickCount();

    std::wcout << L"=== Tab Merge Spy Started ===" << std::endl;
    std::wcout << L"=== Please Drag & Drop Explorer Tabs ===" << std::endl;

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