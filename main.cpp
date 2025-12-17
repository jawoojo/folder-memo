// 👇 유니코드 및 라이브러리 설정
#define UNICODE
#define _UNICODE

#include <dwmapi.h> // DwmGetWindowAttribute 사용을 위해 필요
#pragma comment(lib, "dwmapi.lib") // 라이브러리 링크

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

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

// --- 전역 변수 및 상수 ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;
const int OVERLAY_HEIGHT = 600;
#define IDC_MEMO_EDIT 101
#define WM_UPDATE_PATH (WM_USER + 1)

struct OverlayPair {
    HWND hExplorer;       // 타겟 탐색기
    HWND hOverlay;        // 내 메모장
    std::wstring currentPath;
    // lastPathCheckTick 제거됨 (스레드에서 루프 돌므로 불필요)
};

std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex; // g_overlays 접근 보호
HWINEVENTHOOK g_hHook = NULL;
bool g_running = true; // 스레드 제어용

// --- 파일 입출력 (기존 유지) ---
std::wstring LoadMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return L"";
    std::wstring filePath = folderPath + L"\\system_memo.txt";
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
    std::wstring filePath = folderPath + L"\\system_memo.txt";
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    int len = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, NULL, 0, NULL, NULL);
    if (len > 0) {
        std::vector<char> buf(len);
        WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, buf.data(), len, NULL, NULL);
        DWORD bytesWritten;
        WriteFile(hFile, buf.data(), len - 1, &bytesWritten, NULL);
    }
    CloseHandle(hFile);
}

// --- 오버레이 윈도우 프로시저 ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDC_MEMO_EDIT && HIWORD(wParam) == EN_CHANGE) {
            // 저장 로직 (메인 스레드에서 Path 접근 시 뮤텍스 필요?)
            // 여기서 currentPath는 이미 로드된 시점의 값이므로 안전하게 복사본을 쓰거나, 
            // 잠깐 락을 걸고 가져오는게 좋음. 
            // 하지만 currentPath는 이 창에 바인딩된 것이므로 Find로 찾아야 함.
            std::wstring targetPath = L"";
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) {
                        targetPath = pair.currentPath;
                        break;
                    }
                }
            } // lock 해제

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
    case WM_UPDATE_PATH: { // 배경 스레드에서 경로 바뀌었다고 알려줌
        // wParam: 없음, lParam: 문자열 포인터 (안전하게 새로 로드)
        // 여기서는 다시 Lock을 걸고 경로를 확인하거나, 그냥 다시 로드
        // 간단하게: 해당하는 Overlay를 찾아서 메모 리로드
        std::wstring newPath = L"";
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    newPath = pair.currentPath;
                    break;
                }
            }
        }
        if (!newPath.empty()) {
            std::wstring memo = LoadMemo(newPath);
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
            InvalidateRect(hwnd, NULL, TRUE); // 타이틀바 갱신
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
        MoveWindow(hEdit, 0, 25, rc.right, rc.bottom - 25, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rcTitle = { 0, 0, OVERLAY_WIDTH, 25 };
        HBRUSH brush = CreateSolidBrush(RGB(230, 230, 230));
        FillRect(hdc, &rcTitle, brush);
        DeleteObject(brush);

        SetBkMode(hdc, TRANSPARENT);
        std::wstring msg = L"";
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    msg = pair.currentPath.empty() ? L"경로 없음" : pair.currentPath;
                    break;
                }
            }
        }
        TextOutW(hdc, 5, 5, msg.c_str(), msg.length());
        EndPaint(hwnd, &ps);
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

// --- 공용: UI Automation 경로 가져오기 (비용 큼) ---
// IUIAutomation은 각 스레드마다 별도로 생성해야 함 (STA/MTA 이슈)
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
    // 여기가 가장 느린 부분 (트리 탐색)
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

// --- 백그라운드 스레드: 경로 체크 및 업데이트 ---
void PathCheckerThread() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation* pThreadAutomation = NULL;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&pThreadAutomation);
    
    if (FAILED(hr) || !pThreadAutomation) {
        OutputDebugStringW(L"Background Automation Init Failed\n");
        CoUninitialize();
        return;
    }

    while (g_running) {
        // 1. 검사할 목록 복사 (Lock 시간 최소화)
        std::vector<std::pair<HWND, HWND>> targets;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                targets.push_back({ pair.hExplorer, pair.hOverlay });
            }
        }

        // 2. 각 타겟에 대해 무거운 작업 수행 (Lock 없이)
        for (const auto& t : targets) {
            if (!IsWindow(t.first)) continue; // 죽은 창은 패스

            std::wstring path = GetExplorerPath(pThreadAutomation, t.first);
            
            // 3. 결과 업데이트 (다시 Lock 걸고 확인)
            if (!path.empty()) {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    // 그 사이에 객체가 사라졌을 수도 있으니 다시 찾음
                    for (auto& pair : g_overlays) {
                        if (pair.hExplorer == t.first && pair.hOverlay == t.second) {
                            if (pair.currentPath != path) {
                                pair.currentPath = path;
                                changed = true;
                            }
                            break;
                        }
                    }
                }
                // 변경되었으면 UI 스레드에게 알림
                if (changed) {
                    PostMessage(t.second, WM_UPDATE_PATH, 0, 0);
                }
            }
        }

        // 0.5초 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    pThreadAutomation->Release();
    CoUninitialize();
}

void SyncOverlayPosition(HWND hExplorer, HWND hOverlay) {
    if (!IsWindow(hExplorer)) return;
    RECT rcExp;
    HRESULT res = DwmGetWindowAttribute(hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(hExplorer, &rcExp);

    int x = rcExp.left + 10; 
    int y = rcExp.bottom - OVERLAY_HEIGHT - 10; 

    SetWindowPos(hOverlay, HWND_TOPMOST, x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT, 
                 SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOREDRAW);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) {
                SyncOverlayPosition(pair.hExplorer, pair.hOverlay);
                return;
            }
        }
    }
}

void ManageOverlays(HINSTANCE hInstance) {
    // 1. 죽은 탐색기 정리
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (!IsWindow(it->hExplorer)) {
                DestroyWindow(it->hOverlay);
                it = g_overlays.erase(it);
            } else {
                // 위치 동기화 한번씩 더 (혹시 놓친 것 대비)
                SyncOverlayPosition(it->hExplorer, it->hOverlay);
                ++it;
            }
        }
    }

    // 2. 새로운 탐색기 찾기
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
                    g_overlays.push_back({ hCur, hNew, L"" });
                    SyncOverlayPosition(hCur, hNew);
                }
            }
        }
        hCur = FindWindowExW(NULL, hCur, L"CabinetWClass", NULL);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM Initialization Failed!", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // ⭐ 스레드 시작
    g_running = true;
    std::thread checkerThread(PathCheckerThread);

    SetTimer(NULL, 1, 500, NULL); // 탐색기 관리(생성/파괴)용

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            ManageOverlays(hInstance);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 종료 처리
    g_running = false;
    if (checkerThread.joinable()) checkerThread.join();

    if (g_hHook) UnhookWinEvent(g_hHook);
    CoUninitialize();
    return 0;
}