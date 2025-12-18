// 👇 유니코드 및 라이브러리 설정
#define UNICODE
#define _UNICODE

#include <dwmapi.h> 
#pragma comment(lib, "dwmapi.lib") 

#include <windows.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <codecvt>
#include <cstdio> 
#include <mutex>
#include <filesystem> 

// 🔥 Shell API 관련
#include <shlobj.h>
#include <exdisp.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

namespace fs = std::filesystem;

// --- 상수 및 전역 변수 ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;   
const int OVERLAY_HEIGHT = 600;  
const int MINIMIZED_SIZE = 40;   
const int BTN_SIZE = 25;         

#define IDC_MEMO_EDIT 101
#define WM_UPDATE_PATH (WM_USER + 1)

// 상태 관리 구조체
struct OverlayPair {
    HWND hExplorer;       
    HWND hOverlay;        
    std::wstring currentPath;
    bool isMinimized; 
    bool fileExists; 
};

std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex; 
HWINEVENTHOOK g_hHookObject = NULL; 
HWINEVENTHOOK g_hHookSystem = NULL; 

// --- 디버그 로깅 함수 ---
void Log(const std::string& msg) {
    std::cout << "[LOG] " << msg << std::endl;
}
void LogW(const std::wstring& msg) {
    std::wcout << L"[LOG] " << msg << std::endl;
}

// --- 공용: Explorer 경로 가져오기 (Shell API 방식) ---
std::wstring GetExplorerPath(HWND hExplorer) {
    std::wstring path = L"";
    IShellWindows* psw = NULL;
    
    // ShellWindows 객체 생성
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER, IID_IShellWindows, (void**)&psw))) {
        long count = 0;
        psw->get_Count(&count);
        
        for (long i = 0; i < count; i++) {
            VARIANT v; v.vt = VT_I4; v.lVal = i;
            IDispatch* pDisp = NULL;
            
            if (SUCCEEDED(psw->Item(v, &pDisp))) {
                IWebBrowserApp* pApp = NULL;
                if (SUCCEEDED(pDisp->QueryInterface(IID_IWebBrowserApp, (void**)&pApp))) {
                    HWND hHwnd = NULL;
                    pApp->get_HWND((LONG_PTR*)&hHwnd);
                    
                    if (hHwnd == hExplorer) {
                        BSTR bstrURL = NULL;
                        if (SUCCEEDED(pApp->get_LocationURL(&bstrURL)) && bstrURL) {
                            // URL (file:///...) -> 경로 (C:\...) 변환
                            wchar_t buf[MAX_PATH];
                            DWORD len = MAX_PATH;
                            if (PathCreateFromUrlW(bstrURL, buf, &len, 0) == S_OK) {
                                path = buf;
                            }
                            SysFreeString(bstrURL);
                        }
                    }
                    pApp->Release();
                }
                pDisp->Release();
            }
            if (!path.empty()) break;
        }
        psw->Release();
    }
    return path;
}

// --- 파일 입출력 ---
std::wstring LoadMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return L"";
    
    fs::path p(folderPath);
    p /= L"memo.txt";

    if (!fs::exists(p)) return L"";

    HANDLE hFile = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0) { CloseHandle(hFile); return L""; }

    std::vector<char> buffer(fileSize + 1);
    DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    buffer[bytesRead] = '\0';
    CloseHandle(hFile);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buffer.data(), -1, NULL, 0);
    if (wlen == 0) return L"";
    std::vector<wchar_t> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, buffer.data(), -1, wbuf.data(), wlen);
    return std::wstring(wbuf.data());
}

void SaveMemo(const std::wstring& folderPath, const std::wstring& content) {
    if (folderPath.empty()) return;
    
    fs::path p(folderPath);
    p /= L"memo.txt";

    HANDLE hFile = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    int len = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, NULL, 0, NULL, NULL);
    if (len > 0) {
        std::vector<char> buf(len);
        WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, buf.data(), len, NULL, NULL);
        DWORD bytesWritten;
        WriteFile(hFile, buf.data(), len - 1, &bytesWritten, NULL);
        FlushFileBuffers(hFile); 
    }
    CloseHandle(hFile);
}

void CreateEmptyMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return;
    fs::path p(folderPath);
    p /= L"memo.txt";
    std::ofstream ofs(p);
    ofs.close();
}

// --- 위치 동기화 ---
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;

    RECT rcExp;
    HRESULT res = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(pair.hExplorer, &rcExp);

    bool smallMode = pair.isMinimized || !pair.fileExists;

    int w = smallMode ? MINIMIZED_SIZE : OVERLAY_WIDTH;
    int h = smallMode ? MINIMIZED_SIZE : OVERLAY_HEIGHT;

    int x = rcExp.right - w - 25; 
    int y = rcExp.bottom - h - 10; 

    // 디버그: 위치 계산 확인 (활성화)
    std::cout << "[DEBUG] Sync Pos: " << x << ", " << y << " (" << w << "x" << h << ")" << " RC: " << rcExp.left << "," << rcExp.top << "," << rcExp.right << "," << rcExp.bottom << std::endl;

    // 🔥 [수정] SWP_NOZORDER 제거, SWP_SHOWWINDOW 추가
    SetWindowPos(pair.hOverlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    
    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
    if (hEdit) {
        ShowWindow(hEdit, smallMode ? SW_HIDE : SW_SHOW);
    }
}

// --- 윈도우 프로시저 ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDC_MEMO_EDIT && HIWORD(wParam) == EN_CHANGE) {
            std::wstring targetPath = L"";
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) {
                        targetPath = pair.currentPath;
                        break;
                    }
                }
            }
            if (!targetPath.empty()) {
                int len = GetWindowTextLengthW((HWND)lParam);
                if (len >= 0) {
                    std::vector<wchar_t> buf(len + 1);
                    GetWindowTextW((HWND)lParam, buf.data(), len + 1);
                    SaveMemo(targetPath, std::wstring(buf.data()));
                }
            }
        }
        return 0;
    }
    case WM_UPDATE_PATH: { 
        std::wstring newPath = L"";
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    newPath = pair.currentPath;
                    exists = pair.fileExists;
                    break;
                }
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    SyncOverlayPosition(pair);
                    break;
                }
            }
        }
        
        InvalidateRect(hwnd, NULL, TRUE);

        if (!newPath.empty() && exists) {
            std::wstring memo = LoadMemo(newPath);
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
        }
        return 0;
    }
    case WM_CREATE: {
        Log("Overlay Window Created!");
        CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
        SendDlgItemMessage(hwnd, IDC_MEMO_EDIT, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        if (rc.bottom > BTN_SIZE) {
            MoveWindow(hEdit, 0, BTN_SIZE, rc.right, rc.bottom - BTN_SIZE, TRUE);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);

        bool isMin = false;
        bool hasFile = false;
        
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    isMin = pair.isMinimized;
                    hasFile = pair.fileExists;
                    break;
                }
            }
        }

        if (!hasFile) {
            HBRUSH brush = CreateSolidBrush(RGB(50, 205, 50)); 
            FillRect(hdc, &rcClient, brush);
            DeleteObject(brush);
            
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            
            HFONT hFont = CreateFontW(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
            
            RECT rcText = rcClient;
            DrawTextW(hdc, L"+", -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
        }
        else if (isMin) {
            HBRUSH brush = CreateSolidBrush(RGB(100, 100, 255)); 
            FillRect(hdc, &rcClient, brush);
            DeleteObject(brush);
            
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            TextOutW(hdc, 12, 10, L"O", 1); 
        } 
        else {
            RECT rcTitle = { 0, 0, rcClient.right, BTN_SIZE };
            HBRUSH brush = CreateSolidBrush(RGB(230, 230, 230)); 
            FillRect(hdc, &rcTitle, brush);
            DeleteObject(brush);

            RECT rcClose = { rcClient.right - BTN_SIZE, 0, rcClient.right, BTN_SIZE };
            DrawFrameControl(hdc, &rcClose, DFC_CAPTION, DFCS_CAPTIONCLOSE);

            RECT rcMin = { rcClient.right - BTN_SIZE * 2, 0, rcClient.right - BTN_SIZE, BTN_SIZE };
            DrawFrameControl(hdc, &rcMin, DFC_CAPTION, DFCS_CAPTIONMIN);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        bool hasFile = false;
        std::wstring currentPath = L"";

        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    hasFile = pair.fileExists;
                    currentPath = pair.currentPath;
                    break;
                }
            }
        }

        if (!hasFile) {
            Log("Creating New Memo File...");
            CreateEmptyMemo(currentPath);
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) {
                        pair.fileExists = true;
                        pair.isMinimized = false; 
                        SyncOverlayPosition(pair); 
                        break;
                    }
                }
            }
            PostMessage(hwnd, WM_UPDATE_PATH, 0, 0); 
        }
        else {
            bool isMin = false;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) {
                        isMin = pair.isMinimized;
                        break;
                    }
                }
            }

            if (isMin) {
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    for (auto& pair : g_overlays) {
                        if (pair.hOverlay == hwnd) {
                            pair.isMinimized = false;
                            SyncOverlayPosition(pair);
                            break;
                        }
                    }
                }
                InvalidateRect(hwnd, NULL, TRUE);
            }
            else {
                RECT rcClient;
                GetClientRect(hwnd, &rcClient);
                if (y < BTN_SIZE) {
                    if (x > rcClient.right - BTN_SIZE) {
                        PostQuitMessage(0); 
                    }
                    else if (x > rcClient.right - BTN_SIZE * 2) {
                        {
                            std::lock_guard<std::mutex> lock(g_overlayMutex);
                            for (auto& pair : g_overlays) {
                                if (pair.hOverlay == hwnd) {
                                    pair.isMinimized = true;
                                    SyncOverlayPosition(pair);
                                    break;
                                }
                            }
                        }
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                }
            }
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        SetBkColor(hdcEdit, RGB(255, 255, 255));
        SetTextColor(hdcEdit, RGB(0, 0, 0));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// --- 이벤트 훅 콜백 함수 ---
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        // Log("Event: Location Change");
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) {
                SyncOverlayPosition(pair);
                return;
            }
        }
    }
    else if (event == EVENT_OBJECT_NAMECHANGE) {
        // Log("Event: Name Change");
        HWND hOverlayToUpdate = NULL;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hExplorer == hwnd) {
                    hOverlayToUpdate = pair.hOverlay;
                    break;
                }
            }
        }
        if (hOverlayToUpdate) {
            std::wstring path = GetExplorerPath(hwnd); // UIA 인자 제거
            if (!path.empty()) {
                fs::path p(path);
                p /= L"memo.txt";
                bool exists = fs::exists(p);

                bool needUpdate = false;
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    for (auto& pair : g_overlays) {
                        if (pair.hExplorer == hwnd) {
                            if (pair.currentPath != path || pair.fileExists != exists) {
                                pair.currentPath = path;
                                pair.fileExists = exists;
                                needUpdate = true;
                            }
                            break;
                        }
                    }
                }
                if (needUpdate) PostMessage(hOverlayToUpdate, WM_UPDATE_PATH, 0, 0);
            }
        }
    }
    else if (event == EVENT_SYSTEM_FOREGROUND) {
        // Log("Event: Foreground Change");
        HWND hOverlay = NULL;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hExplorer == hwnd) {
                    hOverlay = pair.hOverlay;
                    break;
                }
            }
        }
        if (hOverlay) {
            // Log("Bringing Overlay to Top");
            SetWindowPos(hOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            PostMessage(hOverlay, WM_UPDATE_PATH, 0, 0);
        }
    }
}

void ManageOverlays(HINSTANCE hInstance) {
    // Log("Timer: ManageOverlays..."); 
    // 1. 정리
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (!IsWindow(it->hExplorer)) {
                Log("Destroying Overlay (Explorer closed)");
                DestroyWindow(it->hOverlay);
                it = g_overlays.erase(it);
            } else {
                SyncOverlayPosition(*it); 
                ++it;
            }
        }
    }

    // 2. 검색
    HWND hCur = FindWindowW(L"CabinetWClass", NULL);
    while (hCur) {
        if (IsWindowVisible(hCur)) {
            bool managed = false;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) {
                    if (pair.hExplorer == hCur) { managed = true; break; }
                }
            }
            
            if (!managed) {
                Log("Found Unmanaged Explorer! Creating Overlay...");
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, 
                    CLASS_NAME, L"Memo", WS_POPUP | WS_VISIBLE, 0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 
                    NULL, NULL, hInstance, NULL);
                SetLayeredWindowAttributes(hNew, 0, 240, LWA_ALPHA);

                if (hNew) {
                    std::wstring path = GetExplorerPath(hCur); // UIA 인자 제거
                    LogW(L"Initial Path: " + path);
                    
                    bool exists = false;
                    if (!path.empty()) {
                        fs::path p(path);
                        p /= L"memo.txt";
                        exists = fs::exists(p);
                    }

                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    OverlayPair newPair = { hCur, hNew, path, false, exists };
                    g_overlays.push_back(newPair);
                    
                    SyncOverlayPosition(newPair);

                    if (exists) {
                         PostMessage(hNew, WM_UPDATE_PATH, 0, 0);
                    }
                } else {
                    Log("Failed to create overlay window! Error: " + std::to_string(GetLastError()));
                }
            }
        }
        hCur = FindWindowExW(NULL, hCur, L"CabinetWClass", NULL);
    }
}

// DPI 인식 함수 타입 정의
typedef HRESULT (STDAPICALLTYPE *SetProcessDpiAwarenessType)(int);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 🔥 [핵심 수정] DPI 인식 설정 (동적 로딩)
    // 컴파일러 버전 호환성을 위해 LoadLibrary 사용
    HMODULE hShCore = LoadLibrary(L"Shcore.dll");
    if (hShCore) {
        SetProcessDpiAwarenessType pSetProcessDpiAwareness = 
            (SetProcessDpiAwarenessType)GetProcAddress(hShCore, "SetProcessDpiAwareness");
        
        if (pSetProcessDpiAwareness) {
            // PROCESS_PER_MONITOR_DPI_AWARE = 2
            pSetProcessDpiAwareness(2); 
            Log("DPI Awareness Set (Per Monitor)");
        } else {
            Log("SetProcessDpiAwareness not found!");
        }
        FreeLibrary(hShCore);
    } else {
        Log("Shcore.dll not found! DPI awareness failed.");
    }

    // 디버그 콘솔 생성
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    std::cout.clear();
    std::wcout.clear();

    Log("Program Started...");

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Log("CoInitializeEx Failed!");
        return 1;
    }

    // Automation 제거됨 (Shell API로 대체)

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) {
        Log("RegisterClassW Failed! Error: " + std::to_string(GetLastError()));
        return 1;
    }

    g_hHookObject = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, 
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_hHookSystem = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, 
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    Log("Hooks Set. Starting Message Loop...");

    SetTimer(NULL, 1, 500, NULL); 

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            ManageOverlays(hInstance);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Log("Exiting...");
    if (g_hHookObject) UnhookWinEvent(g_hHookObject);
    if (g_hHookSystem) UnhookWinEvent(g_hHookSystem);
    CoUninitialize();
    return 0;
}