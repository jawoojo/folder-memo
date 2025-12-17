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
#include <UIAutomation.h>
#include <comdef.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <filesystem> // C++17

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

namespace fs = std::filesystem;

// --- 상수 및 전역 변수 ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;   
const int OVERLAY_HEIGHT = 600;  
const int MINIMIZED_SIZE = 40;   // 버튼 크기 (조금 키움)
const int BTN_SIZE = 25;         

#define IDC_MEMO_EDIT 101
#define WM_UPDATE_PATH (WM_USER + 1)
#define WM_FILE_STATUS_CHANGE (WM_USER + 2)

// 상태 관리 구조체
struct OverlayPair {
    HWND hExplorer;       
    HWND hOverlay;        
    std::wstring currentPath;
    bool isMinimized; 
    bool fileExists; // 파일 존재 여부
};

std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex; 
HWINEVENTHOOK g_hHook = NULL;
bool g_running = true;

// --- 파일 입출력 (std::filesystem 사용) ---
std::wstring LoadMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return L"";
    
    fs::path p(folderPath);
    p /= L"memo.txt";

    if (!fs::exists(p)) return L"";

    // 안전하게 읽기
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
        FlushFileBuffers(hFile); // 디스크 쓰기 강제
    }
    CloseHandle(hFile);
}

void CreateEmptyMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return;
    fs::path p(folderPath);
    p /= L"memo.txt";
    // 빈 파일 생성
    std::ofstream ofs(p);
    ofs.close();
}

// --- 위치 동기화 ---
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;

    RECT rcExp;
    HRESULT res = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(pair.hExplorer, &rcExp);

    // 최소화되었거나 OR 파일이 없으면 -> 작은 버튼 모드
    bool smallMode = pair.isMinimized || !pair.fileExists;

    int w = smallMode ? MINIMIZED_SIZE : OVERLAY_WIDTH;
    int h = smallMode ? MINIMIZED_SIZE : OVERLAY_HEIGHT;

    // 우측 하단
    int x = rcExp.right - w - 25; 
    int y = rcExp.bottom - h - 10; 

    SetWindowPos(pair.hOverlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
    
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
    // 백그라운드 스레드 알림 (경로 변경)
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
        
        // 상태에 따라 위치/크기 재조정 (파일 유무가 바뀌었을 수 있으므로)
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

        // 1. 파일 없음 모드 ([+] 버튼)
        if (!hasFile) {
            HBRUSH brush = CreateSolidBrush(RGB(50, 205, 50)); // 초록색 (생성)
            FillRect(hdc, &rcClient, brush);
            DeleteObject(brush);
            
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            
            // 중앙에 + 표시
            HFONT hFont = CreateFontW(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
            
            RECT rcText = rcClient;
            DrawTextW(hdc, L"+", -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
        }
        // 2. 최소화 모드 (복구 버튼)
        else if (isMin) {
            HBRUSH brush = CreateSolidBrush(RGB(100, 100, 255)); // 파란색
            FillRect(hdc, &rcClient, brush);
            DeleteObject(brush);
            
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            TextOutW(hdc, 12, 10, L"O", 1); 
        } 
        // 3. 일반 모드 (메모장 + 타이틀바)
        else {
            RECT rcTitle = { 0, 0, rcClient.right, BTN_SIZE };
            HBRUSH brush = CreateSolidBrush(RGB(230, 230, 230)); 
            FillRect(hdc, &rcTitle, brush);
            DeleteObject(brush);

            // 닫기 [X]
            RECT rcClose = { rcClient.right - BTN_SIZE, 0, rcClient.right, BTN_SIZE };
            DrawFrameControl(hdc, &rcClose, DFC_CAPTION, DFCS_CAPTIONCLOSE);

            // 최소화 [_]
            RECT rcMin = { rcClient.right - BTN_SIZE * 2, 0, rcClient.right - BTN_SIZE, BTN_SIZE };
            DrawFrameControl(hdc, &rcMin, DFC_CAPTION, DFCS_CAPTIONMIN);

            // * 경로 텍스트 제거됨 (요청사항) *
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        bool isMin = false;
        bool hasFile = false;
        std::wstring currentPath = L"";

        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    isMin = pair.isMinimized;
                    hasFile = pair.fileExists;
                    currentPath = pair.currentPath;
                    break;
                }
            }
        }

        // 1. 파일 생성 모드
        if (!hasFile) {
            CreateEmptyMemo(currentPath); // 파일 생성
            
            // 상태 업데이트
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) {
                        pair.fileExists = true;
                        pair.isMinimized = false; // 생성되면 바로 펼치기
                        SyncOverlayPosition(pair); 
                        break;
                    }
                }
            }
            // 강제 리로드 신호
            PostMessage(hwnd, WM_UPDATE_PATH, 0, 0); 
        }
        // 2. 최소화 상태 -> 복구
        else if (isMin) {
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
        // 3. 일반 상태
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

// --- 공용: UI Automation 경로 가져오기 ---
std::wstring GetExplorerPath(IUIAutomation* pAutomation, HWND hExplorer) {
    if (!pAutomation) return L"";

    IUIAutomationElement* pElement = NULL;
    if (FAILED(pAutomation->ElementFromHandle(hExplorer, &pElement)) || !pElement) return L"";

    IUIAutomationCondition* pCondition = NULL;
    VARIANT varProp;
    varProp.vt = VT_I4;
    varProp.lVal = UIA_EditControlTypeId;
    if (FAILED(pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varProp, &pCondition))) {
        pElement->Release();
        return L"";
    }

    IUIAutomationElement* pFound = NULL;
    pElement->FindFirst(TreeScope_Descendants, pCondition, &pFound);
    std::wstring result = L"";

    if (pFound) {
        IUIAutomationValuePattern* pValuePattern = NULL;
        if (SUCCEEDED(pFound->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern)) && pValuePattern) {
            BSTR bstrValue;
            if (SUCCEEDED(pValuePattern->get_CurrentValue(&bstrValue)) && bstrValue) {
                result = bstrValue;
                SysFreeString(bstrValue);
            }
            pValuePattern->Release();
        }
        pFound->Release();
    }
    pCondition->Release();
    pElement->Release();
    return result;
}

// --- 백그라운드 스레드 ---
void PathCheckerThread() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation* pThreadAutomation = NULL;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&pThreadAutomation);
    
    if (FAILED(hr) || !pThreadAutomation) {
        CoUninitialize();
        return;
    }

    while (g_running) {
        std::vector<std::pair<HWND, HWND>> targets;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                targets.push_back({ pair.hExplorer, pair.hOverlay });
            }
        }

        for (const auto& t : targets) {
            if (!IsWindow(t.first)) continue; 

            std::wstring path = GetExplorerPath(pThreadAutomation, t.first);
            
            // 파일 유무 체크
            bool exists = false;
            if (!path.empty()) {
                fs::path p(path);
                p /= L"memo.txt";
                exists = fs::exists(p);
            }

            if (!path.empty()) {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    for (auto& pair : g_overlays) {
                        if (pair.hExplorer == t.first && pair.hOverlay == t.second) {
                            // 경로가 바뀌었거나 OR 파일 유무 상태가 바뀌었으면 업데이트
                            if (pair.currentPath != path || pair.fileExists != exists) {
                                pair.currentPath = path;
                                pair.fileExists = exists;
                                changed = true;
                            }
                            break;
                        }
                    }
                }
                if (changed) PostMessage(t.second, WM_UPDATE_PATH, 0, 0);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    pThreadAutomation->Release();
    CoUninitialize();
}

// --- 메인 스레드용 이벤트 처리 ---
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) {
                SyncOverlayPosition(pair); 
                return;
            }
        }
    }
}

void ManageOverlays(HINSTANCE hInstance) {
    // 1. 정리
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (!IsWindow(it->hExplorer)) {
                DestroyWindow(it->hOverlay);
                it = g_overlays.erase(it);
            } else {
                SyncOverlayPosition(*it); // 혹시 놓친 위치 업데이트
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
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, 
                    CLASS_NAME, L"Memo", WS_POPUP | WS_VISIBLE, 0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 
                    NULL, NULL, hInstance, NULL);
                SetLayeredWindowAttributes(hNew, 0, 240, LWA_ALPHA);

                if (hNew) {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    // 초기값: 파일없음 가정(스레드가 체크할 때까지), 그러나 UI갱신은 스레드가 trigger하므로 안전.
                    OverlayPair newPair = { hCur, hNew, L"", false, false };
                    g_overlays.push_back(newPair);
                    SyncOverlayPosition(newPair);
                }
            }
        }
        hCur = FindWindowExW(NULL, hCur, L"CabinetWClass", NULL);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_running = true;
    std::thread checkerThread(PathCheckerThread);

    SetTimer(NULL, 1, 500, NULL); 

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            ManageOverlays(hInstance);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    if (checkerThread.joinable()) checkerThread.join();

    if (g_hHook) UnhookWinEvent(g_hHook);
    CoUninitialize();
    return 0;
}