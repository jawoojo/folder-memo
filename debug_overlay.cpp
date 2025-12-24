// ---------------------------------------------------------
// [Tool] Debug Overlay (English Log Edition)
// [Purpose] Measure response latency for Close/Hide events
// ---------------------------------------------------------

#define UNICODE
#define _UNICODE

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#include <windows.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <iostream>
#include <iomanip>

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif

// --- [Constants & Structs] ---
const wchar_t CLASS_NAME[] = L"DebugOverlayClass";
const int OVERLAY_WIDTH = 300;
const int OVERLAY_HEIGHT = 200;

struct OverlayPair {
    HWND hExplorer;
    HWND hOverlay;
};

std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex;
DWORD g_startTime = 0;

// Log Helper
void Log(const std::string& msg, HWND hwnd = NULL) {
    DWORD now = GetTickCount() - g_startTime;
    std::cout << "[" << std::setw(6) << now << "ms] ";
    if (hwnd) std::cout << "[HWND: " << std::hex << hwnd << std::dec << "] ";
    std::cout << msg << std::endl;
}

// Window Procedure (Paint Only)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH hBr = CreateSolidBrush(RGB(255, 255, 0)); // Yellow Box
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);
        
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"Debug Overlay", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Sync Position
void SyncPosition(HWND hOverlay, HWND hExplorer) {
    RECT rc;
    GetWindowRect(hExplorer, &rc);
    SetWindowPos(hOverlay, NULL, rc.right - OVERLAY_WIDTH - 20, rc.bottom - OVERLAY_HEIGHT - 20, 
                 OVERLAY_WIDTH, OVERLAY_HEIGHT, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

// 🔥 [Core] WinEvent Hook
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    // 1. Detect Explorer
    if (event == EVENT_OBJECT_SHOW) {
        if (!IsWindow(hwnd)) return;
        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256) > 0 && wcscmp(className, L"CabinetWClass") == 0) {
            
            Log("EVENT_OBJECT_SHOW (Explorer Detected)", hwnd);

            bool managed = false;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { managed = true; break; }
            }
            if (!managed) {
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_LAYERED, CLASS_NAME, L"DebugMemo", WS_POPUP, 
                                           0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hNew) {
                    SetLayeredWindowAttributes(hNew, 0, 200, LWA_ALPHA);
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        g_overlays.push_back({ hwnd, hNew });
                        SyncPosition(hNew, hwnd);
                    }
                    Log("-> Overlay Created", hNew);
                }
            }
        }
    }
    // 2. Hide / Destroy / Cloaked Detection (Test Point)
    else if (event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_CLOAKED) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            
            if (it->hExplorer == hwnd || !IsWindow(it->hExplorer)) {
                
                std::string evtName = "UNKNOWN";
                if (event == EVENT_OBJECT_HIDE) evtName = "EVENT_OBJECT_HIDE (Hide Signal)";
                else if (event == EVENT_OBJECT_DESTROY) evtName = "EVENT_OBJECT_DESTROY (Destroy Signal)";
                else if (event == EVENT_OBJECT_CLOAKED) evtName = "EVENT_OBJECT_CLOAKED (Cloaked Signal)";
                
                Log("!!! Signal Received: " + evtName, hwnd);

                Log("-> ShowWindow(SW_HIDE) Start...", it->hOverlay);
                ShowWindow(it->hOverlay, SW_HIDE);
                Log("-> ShowWindow(SW_HIDE) End (Check Lag)", it->hOverlay);

                if (event == EVENT_OBJECT_DESTROY || !IsWindow(it->hExplorer)) {
                    Log("-> Explorer Dead. Removing Overlay.");
                    DestroyWindow(it->hOverlay);
                    it = g_overlays.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    // 3. Location Change
    else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (!IsWindow(hwnd)) return;
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { SyncPosition(pair.hOverlay, pair.hExplorer); break; }
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 1. Console for Debug
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    g_startTime = GetTickCount();
    
    std::cout << "=== Debug Overlay Started ===" << std::endl;
    std::cout << "=== 1. Open Explorer (Yellow box will attach) ===" << std::endl;
    std::cout << "=== 2. Close Explorer (Check the Log Time) ===" << std::endl;

    // 2. Register Class
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(255, 255, 0));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // 3. Set Hooks
    HWINEVENTHOOK hHook1 = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook2 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook3 = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hHook1) UnhookWinEvent(hHook1);
    if (hHook2) UnhookWinEvent(hHook2);
    if (hHook3) UnhookWinEvent(hHook3);

    return 0;
}