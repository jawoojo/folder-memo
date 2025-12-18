// ---------------------------------------------------------
// [Project] Explorer Memo Overlay (Stabilized & STA Fixed)
// [Fixed By] Gemini & User
// [Style] Eco-Friendly (Low Overhead), Native API, Robust UX
//
// [Key Fix]
//  - COM 초기화 모델을 MTA -> STA로 변경 (탐색기 멈춤 현상 해결의 핵심)
//  - 비동기 로직 유지로 반응성 확보
// ---------------------------------------------------------

#define UNICODE
#define _UNICODE

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
#include <codecvt>

namespace fs = std::filesystem;

// --- [상수 및 설정] ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;
const int OVERLAY_HEIGHT = 600;
const int MINIMIZED_SIZE = 40;
const int BTN_SIZE = 25;

#define IDC_MEMO_EDIT 101
#define WM_UPDATE_PATH (WM_USER + 1)
#define WM_CHECK_PATH_ASYNC (WM_USER + 2) 

// --- [데이터 구조] ---
struct OverlayPair {
    HWND hExplorer;
    HWND hOverlay;
    std::wstring currentPath;
    bool isMinimized;
    bool fileExists;
};

// --- [전역 자원] ---
std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex;
HWINEVENTHOOK g_hHookObject = NULL;
HWINEVENTHOOK g_hHookSystem = NULL;

// --- [Robust UX] 탐색기 응답성 체크 (Deadlock 방지) ---
bool IsExplorerResponsive(HWND hExplorer) {
    if (!IsWindow(hExplorer)) return false;
    DWORD_PTR dwResult;
    // 50ms만 노크해보고 대답 없으면 "바쁘시구나" 하고 빠짐
    LRESULT lr = SendMessageTimeout(hExplorer, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, &dwResult);
    return (lr != 0);
}

// --- [Lazy Evaluation] 경로 가져오기 ---
std::wstring GetExplorerPath(HWND hExplorer) {
    // [Safety] 탐색기가 바쁘면 COM 호출 자체를 하지 않음
    if (!IsExplorerResponsive(hExplorer)) return L"";

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

                    // [Optimization] 내가 찾는 그 탐색기(hExplorer)가 맞을 때만 무거운 작업을 수행
                    // 다른 탐색기 창(폴더 A)이 바빠도, 지금 폴더 B를 찾고 있다면 여기서 걸러지므로 멈추지 않음
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
                                    if (PathCreateFromUrlW(bstrURL, buf, &len, 0) == S_OK) {
                                        finalPath = buf;
                                    }
                                    SysFreeString(bstrURL);
                                }
                            }
                        }
                    }
                    pApp->Release();
                }
                pDisp->Release();
            }
            // 내 짝꿍 찾았으면 루프 즉시 종료 (CPU 절약)
            if (!finalPath.empty()) break;
        }
        psw->Release();
    }
    return finalPath;
}

// --- [File I/O] ---
std::wstring LoadMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return L"";
    fs::path p(folderPath); p /= L"memo.txt";
    if (!fs::exists(p)) return L"";
    HANDLE hFile = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0) { CloseHandle(hFile); return L""; }
    std::vector<char> buffer(fileSize + 1); DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    buffer[bytesRead] = '\0'; CloseHandle(hFile);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buffer.data(), -1, NULL, 0);
    if (wlen == 0) return L"";
    std::vector<wchar_t> wbuf(wlen); MultiByteToWideChar(CP_UTF8, 0, buffer.data(), -1, wbuf.data(), wlen);
    return std::wstring(wbuf.data());
}

void SaveMemo(const std::wstring& folderPath, const std::wstring& content) {
    if (folderPath.empty()) return;
    fs::path p(folderPath); p /= L"memo.txt";
    HANDLE hFile = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    int len = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, NULL, 0, NULL, NULL);
    if (len > 0) {
        std::vector<char> buf(len); WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, buf.data(), len, NULL, NULL);
        DWORD bytesWritten; WriteFile(hFile, buf.data(), len - 1, &bytesWritten, NULL); FlushFileBuffers(hFile);
    }
    CloseHandle(hFile);
}

void CreateEmptyMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return;
    fs::path p(folderPath); p /= L"memo.txt";
    std::ofstream ofs(p); ofs.close();
}

// --- [Position Sync] ---
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;
    RECT rcExp;
    HRESULT res = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(pair.hExplorer, &rcExp);

    bool smallMode = pair.isMinimized || !pair.fileExists;
    int w = smallMode ? MINIMIZED_SIZE : OVERLAY_WIDTH;
    int h = smallMode ? MINIMIZED_SIZE : OVERLAY_HEIGHT;
    int x = rcExp.right - w - 25;
    int y = rcExp.bottom - h - 25;

    // SWP_NOZORDER: 탐색기 위의 순서를 유지하되 강제로 뺏지 않음 (깜빡임 방지)
    SetWindowPos(pair.hOverlay, NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
    if (hEdit) ShowWindow(hEdit, smallMode ? SW_HIDE : SW_SHOW);
}

// --- [Window Procedure] ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        // [Auto-Save] 타이핑 할 때만 저장 (Low CPU)
        if (LOWORD(wParam) == IDC_MEMO_EDIT && HIWORD(wParam) == EN_CHANGE) {
            std::wstring targetPath = L"";
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { targetPath = pair.currentPath; break; }
                }
            }
            if (!targetPath.empty()) {
                int len = GetWindowTextLengthW((HWND)lParam);
                if (len >= 0) {
                    std::vector<wchar_t> buf(len + 1); GetWindowTextW((HWND)lParam, buf.data(), len + 1);
                    SaveMemo(targetPath, std::wstring(buf.data()));
                }
            }
        }
        return 0;
    }
    // [Async Handler] 이벤트 훅으로부터 안전하게 넘겨받은 작업 처리
    case WM_CHECK_PATH_ASYNC: {
        HWND hExplorer = NULL;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) { hExplorer = pair.hExplorer; break; }
            }
        }
        if (hExplorer && IsExplorerResponsive(hExplorer)) {
            std::wstring path = GetExplorerPath(hExplorer); // 여기서 COM 호출 수행
            if (!path.empty()) {
                fs::path p(path); p /= L"memo.txt";
                bool exists = fs::exists(p);
                bool needUpdate = false;
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    for (auto& pair : g_overlays) {
                        if (pair.hOverlay == hwnd) {
                            if (pair.currentPath != path || pair.fileExists != exists) {
                                pair.currentPath = path; pair.fileExists = exists; needUpdate = true;
                            }
                            break;
                        }
                    }
                }
                if (needUpdate) PostMessage(hwnd, WM_UPDATE_PATH, 0, 0);
            }
        }
        return 0;
    }
    case WM_UPDATE_PATH: {
        std::wstring newPath = L""; bool exists = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    newPath = pair.currentPath; exists = pair.fileExists; SyncOverlayPosition(pair); break;
                }
            }
        }
        InvalidateRect(hwnd, NULL, TRUE);
        if (!newPath.empty() && exists) {
            std::wstring memo = LoadMemo(newPath); SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
        } else if (!newPath.empty() && !exists) {
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, L"");
        }
        return 0;
    }
    case WM_CREATE: {
        CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
        SendDlgItemMessage(hwnd, IDC_MEMO_EDIT, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc); HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        if (rc.bottom > BTN_SIZE) MoveWindow(hEdit, 0, BTN_SIZE, rc.right, rc.bottom - BTN_SIZE, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT rcClient; GetClientRect(hwnd, &rcClient);
        bool isMin = false; bool hasFile = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) { isMin = pair.isMinimized; hasFile = pair.fileExists; break; }
            }
        }
        if (!hasFile) {
            HBRUSH brush = CreateSolidBrush(RGB(50, 205, 50)); FillRect(hdc, &rcClient, brush); DeleteObject(brush);
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255, 255, 255));
            RECT rcText = rcClient; DrawTextW(hdc, L"+", -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (isMin) {
            HBRUSH brush = CreateSolidBrush(RGB(100, 100, 255)); FillRect(hdc, &rcClient, brush); DeleteObject(brush);
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(255, 255, 255)); TextOutW(hdc, 12, 10, L"O", 1);
        } else {
            RECT rcTitle = { 0, 0, rcClient.right, BTN_SIZE };
            HBRUSH brush = CreateSolidBrush(RGB(230, 230, 230)); FillRect(hdc, &rcTitle, brush); DeleteObject(brush);
            RECT rcClose = { rcClient.right - BTN_SIZE, 0, rcClient.right, BTN_SIZE };
            DrawFrameControl(hdc, &rcClose, DFC_CAPTION, DFCS_CAPTIONCLOSE);
            RECT rcMin = { rcClient.right - BTN_SIZE * 2, 0, rcClient.right - BTN_SIZE, BTN_SIZE };
            DrawFrameControl(hdc, &rcMin, DFC_CAPTION, DFCS_CAPTIONMIN);
        }
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam); int y = HIWORD(lParam);
        bool hasFile = false; bool isMin = false; std::wstring currentPath = L"";
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) { hasFile = pair.fileExists; isMin = pair.isMinimized; currentPath = pair.currentPath; break; }
            }
        }
        if (!hasFile) {
            CreateEmptyMemo(currentPath);
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { pair.fileExists = true; pair.isMinimized = false; SyncOverlayPosition(pair); break; }
                }
            }
            PostMessage(hwnd, WM_UPDATE_PATH, 0, 0);
        } else if (isMin) {
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { pair.isMinimized = false; SyncOverlayPosition(pair); break; }
                }
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
                            if (pair.hOverlay == hwnd) { pair.isMinimized = true; SyncOverlayPosition(pair); break; }
                        }
                    }
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam; SetBkColor(hdcEdit, RGB(255, 255, 255)); SetTextColor(hdcEdit, RGB(0, 0, 0));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// --- [Event Hook Callback] ---
// 이벤트를 수신하면 비동기로 작업을 예약합니다. (Blocking 방지)
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) { SyncOverlayPosition(pair); return; }
        }
    }
    else if (event == EVENT_OBJECT_NAMECHANGE) {
        HWND hOverlayToUpdate = NULL;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hExplorer == hwnd) { hOverlayToUpdate = pair.hOverlay; break; }
            }
        }
        // [Async] 직접 처리하지 않고 메시지 큐로 넘김
        if (hOverlayToUpdate) PostMessage(hOverlayToUpdate, WM_CHECK_PATH_ASYNC, 0, 0);
    }
    else if (event == EVENT_SYSTEM_FOREGROUND) {
        HWND hOverlay = NULL;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hExplorer == hwnd) { hOverlay = pair.hOverlay; break; }
            }
        }
        if (hOverlay) {
            SyncOverlayPosition({ hwnd, hOverlay, L"", false, false });
            PostMessage(hOverlay, WM_CHECK_PATH_ASYNC, 0, 0);
        }
    }
}

// --- [Discovery Manager] ---
// 0.5초마다 새 창 확인 (Event Hook에서 놓칠 수 있는 초기화 타이밍 보완)
void ManageOverlays(HINSTANCE hInstance) {
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (!IsWindow(it->hExplorer)) { DestroyWindow(it->hOverlay); it = g_overlays.erase(it); }
            else { ++it; }
        }
    }

    HWND hCur = FindWindowW(L"CabinetWClass", NULL);
    while (hCur) {
        if (IsWindowVisible(hCur)) {
            bool managed = false;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) { if (pair.hExplorer == hCur) { managed = true; break; } }
            }
            if (!managed) {
                if (IsExplorerResponsive(hCur)) {
                    HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_LAYERED, CLASS_NAME, L"Memo", WS_POPUP | WS_VISIBLE, 0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, hCur, NULL, hInstance, NULL);
                    SetLayeredWindowAttributes(hNew, 0, 240, LWA_ALPHA);
                    if (hNew) {
                        std::wstring path = GetExplorerPath(hCur);
                        bool exists = false;
                        if (!path.empty()) { fs::path p(path); p /= L"memo.txt"; exists = fs::exists(p); }
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        OverlayPair newPair = { hCur, hNew, path, false, exists };
                        g_overlays.push_back(newPair);
                        SyncOverlayPosition(newPair);
                        if (exists) PostMessage(hNew, WM_UPDATE_PATH, 0, 0);
                    }
                }
            }
        }
        hCur = FindWindowExW(NULL, hCur, L"CabinetWClass", NULL);
    }
}

typedef HRESULT (STDAPICALLTYPE *SetProcessDpiAwarenessType)(int);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HMODULE hShCore = LoadLibrary(L"Shcore.dll");
    if (hShCore) {
        auto pSetProcessDpiAwareness = (SetProcessDpiAwarenessType)GetProcAddress(hShCore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness) pSetProcessDpiAwareness(2);
        FreeLibrary(hShCore);
    }

    // 🔥 [핵심 수정] MTA(Multithreaded) -> STA(ApartmentThreaded)로 변경
    // 탐색기와 동일한 스레딩 모델을 사용하여 교착 상태(Deadlock)를 방지합니다.
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hHookObject = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hHookSystem = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    SetTimer(NULL, 1, 500, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) ManageOverlays(hInstance);
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hHookObject) UnhookWinEvent(g_hHookObject);
    if (g_hHookSystem) UnhookWinEvent(g_hHookSystem);
    CoUninitialize();
    return 0;
}