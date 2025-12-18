// ---------------------------------------------------------
// [Project] Explorer Memo Overlay (Eco-Friendly Edition)
// [Optimized By] Gemini & User
// [Description]
//  - Windows File Explorer 위에 메모장을 오버레이하는 프로그램
//  - 'Event-Driven' 방식을 사용하여 CPU 사용량을 0%에 수렴하게 최적화
//  - 폴링(Polling)을 배제하고 OS 이벤트를 수신하여 작동
// ---------------------------------------------------------

// 👇 유니코드 설정 (Windows API 표준)
#define UNICODE
#define _UNICODE

// 👇 필수 라이브러리 링크
#pragma comment(lib, "dwmapi.lib")      // 창 위치/속성 감지
#pragma comment(lib, "shlwapi.lib")     // 경로 처리
#pragma comment(lib, "ole32.lib")       // COM 객체 (Shell API)
#pragma comment(lib, "oleaut32.lib")    // BSTR 문자열 처리
#pragma comment(lib, "gdi32.lib")       // 그래픽 그리기 (Paint)
#pragma comment(lib, "uuid.lib")        // COM GUID

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>     // IShellWindows
#include <exdisp.h>     // IWebBrowserApp
#include <shlwapi.h>
#include <vector>
#include <string>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <codecvt>

namespace fs = std::filesystem;

// --- [상수 정의] ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;   
const int OVERLAY_HEIGHT = 600;  
const int MINIMIZED_SIZE = 40;   
const int BTN_SIZE = 25;         

#define IDC_MEMO_EDIT 101
#define WM_UPDATE_PATH (WM_USER + 1)

// --- [데이터 구조] ---
// 각 탐색기 창과 짝이 되는 오버레이 정보를 저장
struct OverlayPair {
    HWND hExplorer;       // 감시 대상 (파일 탐색기)
    HWND hOverlay;        // 내 프로그램 (메모장 창)
    std::wstring currentPath; // 현재 보고 있는 경로
    bool isMinimized;     // 메모장 최소화 여부
    bool fileExists;      // memo.txt 존재 여부
};

// --- [전역 변수] ---
// 여러 스레드(이벤트 훅, 타이머)가 접근하므로 동기화(Mutex) 필수
std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex; 
HWINEVENTHOOK g_hHookObject = NULL; 
HWINEVENTHOOK g_hHookSystem = NULL; 

// --- [핵심 함수 1] 경로 가져오기 (Window Title Matching Strategy) ---
// [Role] 탐색기의 '창 제목'과 일치하는 탭을 찾아서 경로를 반환
// [Advantage] 탭이 여러 개일 때, 현재 눈에 보이는(Active) 탭을 정확히 찾아냄
std::wstring GetExplorerPath(HWND hExplorer) {
    std::wstring finalPath = L"";
    IShellWindows* psw = NULL;
    
    // 1. 현재 탐색기 창의 제목을 가져옵니다. (예: "다운로드")
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
                    
                    // 같은 창(Frame)에 속한 탭인지 확인
                    if (hHwnd == hExplorer) {
                        BSTR bstrName = NULL;
                        // 탭의 이름(폴더명)을 가져옵니다.
                        if (SUCCEEDED(pApp->get_LocationName(&bstrName)) && bstrName) {
                            std::wstring tabName = bstrName;
                            SysFreeString(bstrName);

                            // 🔥 [핵심 로직] 창 제목에 탭 이름이 포함되어 있는지 확인
                            // 예) 창 제목: "다운로드" vs 탭 이름: "다운로드" -> 일치! (Active Tab)
                            // 예) 창 제목: "다운로드" vs 탭 이름: "문서"     -> 불일치 (Background Tab)
                            
                            // (윈도우 설정에 따라 제목 뒤에 "- File Explorer"가 붙을 수 있으므로 포함 여부로 검사)
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
            // 경로를 찾았으면 즉시 종료 (더 이상 뒤져볼 필요 없음)
            if (!finalPath.empty()) break; 
        }
        psw->Release();
    }
    return finalPath;
}

// --- [파일 입출력 헬퍼] ---
std::wstring LoadMemo(const std::wstring& folderPath) {
    if (folderPath.empty()) return L"";
    fs::path p(folderPath); p /= L"memo.txt";
    if (!fs::exists(p)) return L"";

    // 파일 읽기 (UTF-8 -> WCHAR 변환 포함)
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
    fs::path p(folderPath); p /= L"memo.txt";

    HANDLE hFile = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    // WCHAR -> UTF-8 변환 후 저장
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
    fs::path p(folderPath); p /= L"memo.txt";
    std::ofstream ofs(p); ofs.close();
}

// --- [핵심 함수 2] 위치 동기화 ---
// [Role] 탐색기의 현재 위치를 계산하여 메모장 창을 그 옆에 붙임
// [Optimization] SWP_NOZORDER를 제거하여 항상 탐색기 위에 보이도록 강제 (Z-Order 유지)
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;

    RECT rcExp;
    // DWM API를 써야 그림자/투명 영역 제외한 실제 창 크기를 구함
    HRESULT res = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(pair.hExplorer, &rcExp);

    bool smallMode = pair.isMinimized || !pair.fileExists;
    int w = smallMode ? MINIMIZED_SIZE : OVERLAY_WIDTH;
    int h = smallMode ? MINIMIZED_SIZE : OVERLAY_HEIGHT;

    // 우측 하단 좌표 계산
    int x = rcExp.right - w - 25; 
    int y = rcExp.bottom - h - 10; 

    // [수정] HWND_TOPMOST를 HWND_TOP (혹은 아예 순서 변경 없음)으로 변경
    // SWP_NOZORDER를 넣어서 "순서는 윈도우가 알아서 관리하게 놔두고 위치만 옮겨"라고 합니다.
    // 주인(탐색기)이 움직이면 OS가 알아서 메모장을 그 위에 그려줍니다.
    SetWindowPos(pair.hOverlay, NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    
    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
    if (hEdit) {
        ShowWindow(hEdit, smallMode ? SW_HIDE : SW_SHOW);
    }
}

// --- [윈도우 프로시저] 메시지 처리 ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        // 메모 내용이 변경되면 즉시 파일 저장 (Auto-Save)
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
    // [Custom Message] 경로 변경 알림 수신 시 UI 업데이트
    case WM_UPDATE_PATH: { 
        std::wstring newPath = L"";
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    newPath = pair.currentPath;
                    exists = pair.fileExists;
                    SyncOverlayPosition(pair); // 상태 변경 시 위치/크기 재조정
                    break;
                }
            }
        }
        
        InvalidateRect(hwnd, NULL, TRUE); // 화면 다시 그리기 요청

        if (!newPath.empty() && exists) {
            std::wstring memo = LoadMemo(newPath);
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
        }
        return 0;
    }
    case WM_CREATE: {
        // 메모 입력창(Edit Control) 생성
        CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        
        // 폰트 설정 (맑은 고딕)
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
        SendDlgItemMessage(hwnd, IDC_MEMO_EDIT, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_SIZE: {
        // 창 크기가 변하면 에디트 컨트롤 크기도 조절
        RECT rc; GetClientRect(hwnd, &rc);
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        if (rc.bottom > BTN_SIZE) {
            MoveWindow(hEdit, 0, BTN_SIZE, rc.right, rc.bottom - BTN_SIZE, TRUE);
        }
        return 0;
    }
    case WM_PAINT: {
        // UI 그리기 (초록색 +, 파란색 O, 타이틀바 등)
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rcClient; GetClientRect(hwnd, &rcClient);

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
            // [State] 파일 없음 -> 초록색 [+] 버튼
            HBRUSH brush = CreateSolidBrush(RGB(50, 205, 50)); 
            FillRect(hdc, &rcClient, brush);
            DeleteObject(brush);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT rcText = rcClient;
            DrawTextW(hdc, L"+", -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        else if (isMin) {
            // [State] 최소화 -> 파란색 [O] 버튼
            HBRUSH brush = CreateSolidBrush(RGB(100, 100, 255)); 
            FillRect(hdc, &rcClient, brush);
            DeleteObject(brush);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            TextOutW(hdc, 12, 10, L"O", 1); 
        } 
        else {
            // [State] 일반 모드 -> 상단 타이틀바 그리기
            RECT rcTitle = { 0, 0, rcClient.right, BTN_SIZE };
            HBRUSH brush = CreateSolidBrush(RGB(230, 230, 230)); 
            FillRect(hdc, &rcTitle, brush);
            DeleteObject(brush);

            // 닫기(X), 최소화(_) 버튼 드로잉 (Windows API 기본 제공)
            RECT rcClose = { rcClient.right - BTN_SIZE, 0, rcClient.right, BTN_SIZE };
            DrawFrameControl(hdc, &rcClose, DFC_CAPTION, DFCS_CAPTIONCLOSE);
            RECT rcMin = { rcClient.right - BTN_SIZE * 2, 0, rcClient.right - BTN_SIZE, BTN_SIZE };
            DrawFrameControl(hdc, &rcMin, DFC_CAPTION, DFCS_CAPTIONMIN);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // 클릭 이벤트 처리 (버튼 클릭 감지)
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        
        // 현재 상태 스냅샷
        bool hasFile = false;
        bool isMin = false;
        std::wstring currentPath = L"";
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    hasFile = pair.fileExists;
                    isMin = pair.isMinimized;
                    currentPath = pair.currentPath;
                    break;
                }
            }
        }

        // 1. [+] 버튼 클릭: 파일 생성
        if (!hasFile) {
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
        // 2. [O] 버튼 클릭: 최소화 해제
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
        // 3. 타이틀바 버튼 클릭 (X, _)
        else {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            if (y < BTN_SIZE) { // 타이틀바 영역
                if (x > rcClient.right - BTN_SIZE) {
                    PostQuitMessage(0); // [X] 클릭 시 프로그램 종료
                }
                else if (x > rcClient.right - BTN_SIZE * 2) { // [_] 클릭 시 최소화
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

// --- [핵심 함수 3] 이벤트 훅 콜백 (Event-Driven Logic) ---
// [Role] 윈도우(OS)에서 발생하는 특정 사건을 감지하여 반응
// [Efficiency] 루프를 돌지 않고, 사건이 발생했을 때만 호출되므로 CPU 사용량 최소화
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW) return;

    // 1. 창 위치 변경 (이동, 리사이징)
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) {
                SyncOverlayPosition(pair); // 메모장도 따라감 (가벼운 연산)
                return;
            }
        }
    }
    // 2. 창 이름 변경 (폴더 이동 시 발생)
    else if (event == EVENT_OBJECT_NAMECHANGE) {
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
        // 관리 중인 탐색기라면 경로 확인 (무거운 연산은 여기서만 수행)
        if (hOverlayToUpdate) {
            std::wstring path = GetExplorerPath(hwnd);
            if (!path.empty()) {
                fs::path p(path); p /= L"memo.txt";
                bool exists = fs::exists(p);

                bool needUpdate = false;
                {
                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    for (auto& pair : g_overlays) {
                        if (pair.hExplorer == hwnd) {
                            // 실제 데이터가 변했을 때만 업데이트 요청
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
    // 3. 창 활성화 변경 (Alt+Tab, 클릭 등)
    else if (event == EVENT_SYSTEM_FOREGROUND) {
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
        
        // 🔥 [수정됨] 범인 검거 및 삭제 완료!
        // 기존: SetWindowPos(..., HWND_TOPMOST, ...) <-- 이 녀석이 범인입니다.
        // 수정: 윈도우 OS의 족보 시스템(Owner-Owned)을 믿고, 우리는 아무것도 하지 않거나
        //       위치 싱크만 살짝 맞춰줍니다. (TopMost 강제 적용 금지)
        
        if (hOverlay) {
            // 위치만 한번 맞춰줌 (혹시 모르니)
            SyncOverlayPosition({ hwnd, hOverlay, L"", false, false }); 
            // 경로 업데이트 체크
            PostMessage(hOverlay, WM_UPDATE_PATH, 0, 0);
        }
    }
}

// --- [관리 함수] 새 탐색기 발견 및 죽은 창 정리 ---
// [Trigger] 0.5초마다 타이머에 의해 실행
// [Anti-Polling] 기존 창에 대해서는 아무 작업도 하지 않음 (Skip)
void ManageOverlays(HINSTANCE hInstance) {
    // 1. 죽은 창 정리 (Cleanup)
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (!IsWindow(it->hExplorer)) {
                DestroyWindow(it->hOverlay);
                it = g_overlays.erase(it);
            } else {
                // [PASS] 살아있는 창은 건드리지 않음 (이벤트 훅이 관리함)
                ++it;
            }
        }
    }

    // 2. 새 탐색기 검색 (Discovery)
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
            
            // [NEW] 관리되지 않는 새 탐색기 발견!
            if (!managed) {
                // [수정] 부모 윈도우 인자에 hCur(탐색기 핸들)를 넣습니다.
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_LAYERED, // WS_EX_TOPMOST 제거!
                    CLASS_NAME, L"Memo", WS_POPUP | WS_VISIBLE, 0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 
                    hCur, // 🔥 여기가 핵심! (NULL -> hCur) 이 메모장의 주인은 탐색기라고 선언
                    NULL, hInstance, NULL);
                SetLayeredWindowAttributes(hNew, 0, 240, LWA_ALPHA);

                if (hNew) {
                    // 최초 1회만 경로 및 위치 설정
                    std::wstring path = GetExplorerPath(hCur);
                    bool exists = false;
                    if (!path.empty()) {
                        fs::path p(path); p /= L"memo.txt";
                        exists = fs::exists(p);
                    }

                    std::lock_guard<std::mutex> lock(g_overlayMutex);
                    OverlayPair newPair = { hCur, hNew, path, false, exists };
                    g_overlays.push_back(newPair);
                    
                    SyncOverlayPosition(newPair); // 초기 위치 잡기

                    if (exists) PostMessage(hNew, WM_UPDATE_PATH, 0, 0);
                }
            }
        }
        hCur = FindWindowExW(NULL, hCur, L"CabinetWClass", NULL);
    }
}

// DPI 함수 포인터 정의
typedef HRESULT (STDAPICALLTYPE *SetProcessDpiAwarenessType)(int);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // [DPI Awareness] 고해상도 모니터 대응
    // Shcore.dll을 동적으로 로드하여 실행 (없으면 무시)
    HMODULE hShCore = LoadLibrary(L"Shcore.dll");
    if (hShCore) {
        auto pSetProcessDpiAwareness = (SetProcessDpiAwarenessType)GetProcAddress(hShCore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness) {
            pSetProcessDpiAwareness(2); // PROCESS_PER_MONITOR_DPI_AWARE
        }
        FreeLibrary(hShCore);
    }

    // COM 초기화 (Shell API 사용을 위해 필수)
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // 윈도우 클래스 등록
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // [Hooks] 이벤트 리스너 설치 (가장 중요한 부분)
    // 1. 객체 변경 (이동, 이름변경) 감지
    g_hHookObject = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, 
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 2. 시스템 상태 (포커스) 감지
    g_hHookSystem = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, 
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 타이머 시작 (새 창 발견용 - 0.5초 간격)
    SetTimer(NULL, 1, 500, NULL); 

    // 메시지 루프
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            ManageOverlays(hInstance);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 종료 및 정리
    if (g_hHookObject) UnhookWinEvent(g_hHookObject);
    if (g_hHookSystem) UnhookWinEvent(g_hHookSystem);
    CoUninitialize();
    return 0;
}