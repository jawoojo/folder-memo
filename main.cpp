// ---------------------------------------------------------
// [Project] Explorer Memo Overlay (Final Async Edition)
// [Optimized By] Gemini & User
// [Description]
//  - Windows File Explorer 위에 메모장을 오버레이하는 프로그램
//  - 'Event-Driven' + 'Async Thread' 방식을 사용하여
//    탐색기가 응답 없음 상태일 때도 내 프로그램은 절대 멈추지 않음.
// ---------------------------------------------------------

// 👇 유니코드 설정
#define UNICODE
#define _UNICODE

// 👇 필수 라이브러리 링크
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

// --- [상수 정의 업데이트] ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;       
const int OVERLAY_HEIGHT = 600;      
const int EXPANDED_WIDTH = 600;      
const int EXPANDED_HEIGHT = 900;     
const int MINIMIZED_SIZE = 50;       // 🔥 [디자인] 최소화 크기 40 -> 50으로 변경
const int BTN_SIZE = 25;             
const int DEFAULT_FONT_SIZE = 20;    

// 🔥 [디자인] 배경색 정의 (눈이 편한 연회색 #F3F3F3)
const COLORREF BG_COLOR = RGB(243, 243, 243);

#define IDC_MEMO_EDIT 101
#define WM_UPDATE_UI_FromThread (WM_USER + 2)

// --- [데이터 구조] ---
struct OverlayPair {
    HWND hExplorer;
    HWND hOverlay;
    std::wstring currentPath;
    bool isMinimized;
    bool isExpanded;
    bool fileExists;
    int currentFontSize;
};

// --- [전역 변수] ---
std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex;
HWINEVENTHOOK g_hHookObject = NULL;
HWINEVENTHOOK g_hHookSystem = NULL;

// --- [헬퍼 함수: 폰트 적용] ---
void UpdateMemoFont(HWND hEdit, int fontSize) {
    if (!hEdit) return;
    HFONT hNewFont = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
    SendMessage(hEdit, WM_SETFONT, (WPARAM)hNewFont, TRUE);
}

// --- [헬퍼 함수] ---
void SyncOverlayPosition(const OverlayPair& pair); 

// --- [핵심 함수 1] 경로 가져오기 (COM) ---
std::wstring GetExplorerPath(HWND hExplorer) {
    std::wstring finalPath = L"";
    IShellWindows* psw = NULL;
    wchar_t szTitle[MAX_PATH] = { 0 };
    GetWindowTextW(hExplorer, szTitle, MAX_PATH);
    std::wstring windowTitle = szTitle;

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
                        BSTR bstrName = NULL;
                        if (SUCCEEDED(pApp->get_LocationName(&bstrName)) && bstrName) {
                            std::wstring tabName = bstrName;
                            SysFreeString(bstrName);
                            if (windowTitle.find(tabName) != std::wstring::npos) {
                                BSTR bstrURL = NULL;
                                if (SUCCEEDED(pApp->get_LocationURL(&bstrURL)) && bstrURL) {
                                    wchar_t buf[MAX_PATH];
                                    DWORD len = MAX_PATH;
                                    if (PathCreateFromUrlW(bstrURL, buf, &len, 0) == S_OK) finalPath = buf;
                                    SysFreeString(bstrURL);
                                }
                            }
                        }
                    }
                    pApp->Release();
                }
                pDisp->Release();
            }
            if (!finalPath.empty()) break;
        }
        psw->Release();
    }
    return finalPath;
}

// --- [파일 입출력] ---
std::wstring LoadMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return L"";
    fs::path p(folderPath); p /= L"folder_memo.txt";
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
    fs::path p(folderPath); p /= L"folder_memo.txt";
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
    fs::path p(folderPath); p /= L"folder_memo.txt";
    std::ofstream ofs(p); ofs.close();
}

// --- [핵심 함수 2] 위치 동기화 ---
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;
    RECT rcExp;
    HRESULT res = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(pair.hExplorer, &rcExp);

    bool smallMode = pair.isMinimized;
    int targetW = smallMode ? MINIMIZED_SIZE : (pair.isExpanded ? EXPANDED_WIDTH : OVERLAY_WIDTH);
    int targetH = smallMode ? MINIMIZED_SIZE : (pair.isExpanded ? EXPANDED_HEIGHT : OVERLAY_HEIGHT);

    // 탐색기 우측 하단 기준 좌표 계산
    int x = rcExp.right - targetW - 25;
    int y = rcExp.bottom - targetH - 25;

    SetWindowPos(pair.hOverlay, NULL, x, y, targetW, targetH, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
    if (hEdit) ShowWindow(hEdit, smallMode ? SW_HIDE : SW_SHOW);
}

// --- [핵심 함수 3] 비동기 작업 스레드 ---
void PathFinderThread(HWND hOverlay, HWND hExplorer) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    std::wstring foundPath = L"";
    for (int i = 0; i < 5; i++) {
        if (!IsWindow(hExplorer)) break;
        foundPath = GetExplorerPath(hExplorer);
        if (!foundPath.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (IsWindow(hOverlay)) {
        bool exists = false;
        if (!foundPath.empty()) {
            fs::path p(foundPath); p /= L"folder_memo.txt";
            exists = fs::exists(p);
        }
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (auto& pair : g_overlays) {
                if (pair.hOverlay == hOverlay) {
                    pair.currentPath = foundPath;
                    pair.fileExists = exists;
                    if (exists) pair.isMinimized = false;
                    break;
                }
            }
        }
        PostMessage(hOverlay, WM_UPDATE_UI_FromThread, (WPARAM)exists, 0);
    }
    CoUninitialize();
}

// --- [윈도우 프로시저 (디자인 수정 완료)] ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_UPDATE_UI_FromThread: {
        bool exists = (bool)wParam;
        std::wstring currentPath = L"";
        int currentFontSize = DEFAULT_FONT_SIZE;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    currentPath = pair.currentPath;
                    pair.fileExists = exists;
                    currentFontSize = pair.currentFontSize; 
                    SyncOverlayPosition(pair);
                    break;
                }
            }
        }
        InvalidateRect(hwnd, NULL, TRUE);
        if (exists && !currentPath.empty()) {
            std::wstring memo = LoadMemo(currentPath);
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
        } else {
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, L"");
        }
        UpdateMemoFont(GetDlgItem(hwnd, IDC_MEMO_EDIT), currentFontSize);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (LOWORD(wParam) & MK_CONTROL) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int change = (delta > 0) ? 2 : -2; 
            int newSize = DEFAULT_FONT_SIZE;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) {
                        pair.currentFontSize += change;
                        if (pair.currentFontSize < 8) pair.currentFontSize = 8;
                        if (pair.currentFontSize > 72) pair.currentFontSize = 72;
                        newSize = pair.currentFontSize;
                        break;
                    }
                }
            }
            UpdateMemoFont(GetDlgItem(hwnd, IDC_MEMO_EDIT), newSize);
            return 0; 
        }
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        if (hEdit) SendMessage(hEdit, uMsg, wParam, lParam);
        break; 
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == IDC_MEMO_EDIT && HIWORD(wParam) == EN_CHANGE) {
            std::wstring targetPath = L"";
            bool* pFileExists = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { targetPath = pair.currentPath; pFileExists = &pair.fileExists; break; }
                }
            }
            if (!targetPath.empty()) {
                if (pFileExists && !(*pFileExists)) {
                    CreateEmptyMemo(targetPath);
                    *pFileExists = true;
                }
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

    case WM_CREATE: {
        CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        UpdateMemoFont(GetDlgItem(hwnd, IDC_MEMO_EDIT), DEFAULT_FONT_SIZE);
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        if (rc.bottom > BTN_SIZE) {
            MoveWindow(hEdit, 1, BTN_SIZE + 1, rc.right - 2, rc.bottom - BTN_SIZE - 2, TRUE);
            SendMessage(hEdit, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        bool isMin = false, isExp = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) if (pair.hOverlay == hwnd) { isMin = pair.isMinimized; isExp = pair.isExpanded; break; }
        }

        // 🔥 [디자인 수정] 배경색을 #F3F3F3(연회색)으로 채움
        HBRUSH hBgBrush = CreateSolidBrush(BG_COLOR);
        FillRect(hdc, &rcClient, hBgBrush);
        DeleteObject(hBgBrush);

        if (isMin) {
            // 🔥 [디자인 수정] 최소화 아이콘 (▤) 그리기
            // 50px 박스 안에 꽉 차게 폰트 크기 조정
            HFONT hIconFont = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
            
            HFONT hOldFont = (HFONT)SelectObject(hdc, hIconFont);
            SetBkMode(hdc, TRANSPARENT); 
            SetTextColor(hdc, RGB(50, 50, 50)); 
            
            // 중앙 정렬하여 ▤ 출력
            RECT rcIcon = rcClient;
            DrawTextW(hdc, L"▤", -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            SelectObject(hdc, hOldFont);
            DeleteObject(hIconFont);

            // 최소화 상태에서도 테두리 그리기
            HBRUSH hBorderBrush = CreateSolidBrush(RGB(100, 100, 100)); 
            FrameRect(hdc, &rcClient, hBorderBrush);
            DeleteObject(hBorderBrush);

        } else {
            // --- 플랫 버튼 그리기 ---
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0)); 
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

            int btnW = BTN_SIZE;
            int right = rcClient.right;
            
            // X
            MoveToEx(hdc, right - btnW + 8, 8, NULL); LineTo(hdc, right - 8, btnW - 8);
            MoveToEx(hdc, right - 8, 8, NULL); LineTo(hdc, right - btnW + 8, btnW - 8);

            // ㅁ
            int expRight = right - btnW;
            Rectangle(hdc, expRight - btnW + 8, 8, expRight - 8, btnW - 8);

            // _
            int minRight = expRight - btnW;
            MoveToEx(hdc, minRight - btnW + 8, btnW - 8, NULL); LineTo(hdc, minRight - 8, btnW - 8);

            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);

            // --- 테두리 & 라인 ---
            HBRUSH hBorderBrush = CreateSolidBrush(RGB(100, 100, 100)); 
            FrameRect(hdc, &rcClient, hBorderBrush); 
            
            RECT rcLine = { 0, BTN_SIZE, rcClient.right, BTN_SIZE + 1 };
            HBRUSH hLineBrush = CreateSolidBrush(RGB(200, 200, 200));
            FillRect(hdc, &rcLine, hLineBrush);
            
            DeleteObject(hBorderBrush);
            DeleteObject(hLineBrush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam); int y = HIWORD(lParam);
        bool isMin = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) if (pair.hOverlay == hwnd) { isMin = pair.isMinimized; break; }
        }

        if (isMin) {
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) if (pair.hOverlay == hwnd) { pair.isMinimized = false; SyncOverlayPosition(pair); break; }
            }
            InvalidateRect(hwnd, NULL, TRUE);
        } else {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            if (y < BTN_SIZE) { 
                if (x > rcClient.right - BTN_SIZE) PostQuitMessage(0);
                else if (x > rcClient.right - BTN_SIZE * 2) {
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        for (auto& pair : g_overlays) {
                            if (pair.hOverlay == hwnd) { pair.isExpanded = !pair.isExpanded; SyncOverlayPosition(pair); break; }
                        }
                    }
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                else if (x > rcClient.right - BTN_SIZE * 3) {
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        for (auto& pair : g_overlays) if (pair.hOverlay == hwnd) { pair.isMinimized = true; SyncOverlayPosition(pair); break; }
                    }
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
        }
        return 0;
    }
    
    // 🔥 [디자인 수정] 에디트 컨트롤(메모장) 배경색 변경 (#F3F3F3)
    case WM_CTLCOLOREDIT: { 
        HDC hdcEdit = (HDC)wParam; 
        SetBkColor(hdcEdit, BG_COLOR); 
        SetTextColor(hdcEdit, RGB(0, 0, 0));
        // 배경색 브러시를 생성해서 반환 (static을 써서 계속 재사용)
        static HBRUSH hEditBgBrush = CreateSolidBrush(BG_COLOR);
        return (LRESULT)hEditBgBrush; 
    }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// --- [Event Hook] ---
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    if (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) {
        if (!IsWindow(hwnd)) return;
        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256) > 0 && wcscmp(className, L"CabinetWClass") == 0) {
            bool managed = false;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { managed = true; break; }
            }
            if (!managed) {
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_LAYERED, CLASS_NAME, L"Memo", WS_POPUP | WS_VISIBLE, 
                                           0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hNew) {
                    SetLayeredWindowAttributes(hNew, 0, 200, LWA_ALPHA);
                    UpdateMemoFont(GetDlgItem(hNew, IDC_MEMO_EDIT), DEFAULT_FONT_SIZE);
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        g_overlays.push_back({ hwnd, hNew, L"", false, false, false, DEFAULT_FONT_SIZE });
                        SyncOverlayPosition(g_overlays.back());
                    }
                    std::thread(PathFinderThread, hNew, hwnd).detach();
                }
            }
        }
    }
    else if (event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (it->hExplorer == hwnd || !IsWindow(it->hExplorer)) {
                ShowWindow(it->hOverlay, SW_HIDE);
                if (event == EVENT_OBJECT_DESTROY || !IsWindow(it->hExplorer)) {
                    DestroyWindow(it->hOverlay); 
                    it = g_overlays.erase(it); 
                    continue; 
                }
            }
            ++it;
        }
    }
    else if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_FOREGROUND) {
        if (!IsWindow(hwnd)) return; 
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { SyncOverlayPosition(pair); break; }
    }
    else if (event == EVENT_OBJECT_NAMECHANGE) {
        if (!IsWindow(hwnd)) return;
        HWND hOverlay = NULL;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { hOverlay = pair.hOverlay; break; }
        }
        if (hOverlay) std::thread(PathFinderThread, hOverlay, hwnd).detach();
    }
}

// --- [Main] ---
typedef HRESULT (STDAPICALLTYPE *SetProcessDpiAwarenessType)(int);
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HMODULE hShCore = LoadLibrary(L"Shcore.dll");
    if (hShCore) {
        auto pSetProcessDpiAwareness = (SetProcessDpiAwarenessType)GetProcAddress(hShCore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness) pSetProcessDpiAwareness(2);
        FreeLibrary(hShCore);
    }
    
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    // 🔥 [디자인 수정] 기본 배경색을 회색(#F3F3F3)으로 설정
    wc.hbrBackground = CreateSolidBrush(BG_COLOR); 
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWINEVENTHOOK hHook1 = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook2 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook3 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_hHookObject = hHook1;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessage(msg.hwnd, EM_SETSEL, 0, -1);
            continue; 
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hHook1) UnhookWinEvent(hHook1);
    if (hHook2) UnhookWinEvent(hHook2);
    if (hHook3) UnhookWinEvent(hHook3);
    
    CoUninitialize();
    return 0;
}