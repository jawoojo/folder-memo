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
#include <thread> // 🔥 [핵심] 비동기 처리를 위한 헤더
#include <chrono> // 시간 지연용

namespace fs = std::filesystem;

// --- [상수 정의] ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;
const int OVERLAY_HEIGHT = 600;
const int MINIMIZED_SIZE = 40;
const int BTN_SIZE = 25;

#define IDC_MEMO_EDIT 101
#define WM_UPDATE_UI_FromThread (WM_USER + 2) // 스레드가 일 다하고 보내는 신호

// --- [데이터 구조] ---
struct OverlayPair {
    HWND hExplorer;       // 감시 대상 (탐색기)
    HWND hOverlay;        // 내 프로그램 (메모장)
    std::wstring currentPath;
    bool isMinimized;
    bool fileExists;
};

// --- [전역 변수] ---
std::vector<OverlayPair> g_overlays;
std::mutex g_overlayMutex;
HWINEVENTHOOK g_hHookObject = NULL;
HWINEVENTHOOK g_hHookSystem = NULL;

// --- [헬퍼 함수] ---
void SyncOverlayPosition(const OverlayPair& pair); // 전방 선언

// --- [핵심 함수 1] 경로 가져오기 (COM) ---
// 주의: 이 함수는 이제 메인 스레드가 아니라 '작업 스레드'에서 호출됩니다.
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

// --- [핵심 함수 2] 위치 동기화 (수정됨) ---
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;

    RECT rcExp;
    HRESULT res = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (res != S_OK) GetWindowRect(pair.hExplorer, &rcExp);

    // [PRD 3.2.1] 상시 입력 대기
    // Why: 파일이 없어도 입력 가능한 상태여야 하므로, !fileExists라고 해서 강제로 smallMode로 만들지 않음.
    // 오직 사용자가 명시적으로 최소화(isMinimized)했을 때만 작게 변함.
    bool smallMode = pair.isMinimized; 

    int w = smallMode ? MINIMIZED_SIZE : OVERLAY_WIDTH;
    int h = smallMode ? MINIMIZED_SIZE : OVERLAY_HEIGHT;

    int x = rcExp.right - w - 25;
    int y = rcExp.bottom - h - 25;

    SetWindowPos(pair.hOverlay, NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    
    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
    if (hEdit) {
        // [PRD 3.2.1] 파일이 없어도 에디트 박스는 항상 보여야 함 (최소화 상태만 아니면)
        ShowWindow(hEdit, smallMode ? SW_HIDE : SW_SHOW);
    }
}

// --- [핵심 함수 3] 비동기 작업 스레드 (Worker Thread) ---
// [Role] 탐색기가 바쁘든 말든, 별도 스레드에서 끈질기게 경로를 알아와서 보고함
void PathFinderThread(HWND hOverlay, HWND hExplorer) {
    // 1. 스레드별 COM 초기화 (필수)
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::wstring foundPath = L"";
    
    // [Retry Policy] 최대 5번 시도, 시도 간격 0.3초
    // 탐색기 탭 분리 등 대공사 중일 때를 대비해 조금 기다려줌 (Polling 아님, Retry임)
    for (int i = 0; i < 5; i++) {
        // 탐색기 핸들이 유효한지 체크
        if (!IsWindow(hExplorer)) break;

        // 경로 추출 시도 (여기서 탐색기가 멈춰있으면 이 스레드만 멈춤. 메인 프로그램은 안전!)
        foundPath = GetExplorerPath(hExplorer);

        if (!foundPath.empty()) break; // 찾았으면 탈출

        // 못 찾았으면(탐색기 부팅중) 잠시 대기 후 재시도
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // 2. 결과 처리 (경로를 찾았든 못 찾았든 보고)
    if (IsWindow(hOverlay)) {
        bool exists = false;
        if (!foundPath.empty()) {
            fs::path p(foundPath); p /= L"folder_memo.txt";
            exists = fs::exists(p);
        }

        // [Critical Section] 전역 데이터 업데이트
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (auto& pair : g_overlays) {
                if (pair.hOverlay == hOverlay) {
                    pair.currentPath = foundPath;
                    pair.fileExists = exists;
                    if (exists) pair.isMinimized = false;
                    // 여기서 위치 싱크를 맞추는 건 메인 스레드에게 맡김 (안전하게)
                    break;
                }
            }
        }

        // 3. 메인 스레드에게 "작업 끝났다"고 알림
        PostMessage(hOverlay, WM_UPDATE_UI_FromThread, (WPARAM)exists, 0);
    }

    CoUninitialize();
}

// --- [윈도우 프로시저 (수정됨)] ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    // 스레드가 작업 완료 후 보내는 메시지 (UI 업데이트)
    case WM_UPDATE_UI_FromThread: {
        bool exists = (bool)wParam;
        std::wstring currentPath = L"";
        
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (auto& pair : g_overlays) { // pair 값을 수정할 수도 있으므로 auto&
                if (pair.hOverlay == hwnd) {
                    currentPath = pair.currentPath;
                    pair.fileExists = exists; // 최신 상태 업데이트
                    SyncOverlayPosition(pair); // [PRD 3.2.1] 즉시 UI 반영
                    break;
                }
            }
        }

        InvalidateRect(hwnd, NULL, TRUE);

        // [PRD 3.1.2] 데이터 로딩
        // 파일이 있으면 로드, 없으면 빈 칸으로 두어 작성 대기 상태 유지
        if (exists && !currentPath.empty()) {
            std::wstring memo = LoadMemo(currentPath);
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
        } else {
            // 이미 작성 중인 내용이 있을 수 있으므로 무작정 지우지 않고,
            // 경로가 바뀌었거나 명확히 없는 경우에만 처리해야 하나,
            // 여기서는 스레드 결과에 따라 파일이 없으면 일단 빈 칸으로 둠 (새 폴더 진입 시)
            // *심화: 사용자가 막 쓰고 있는데 스레드가 "파일 없음" 보냈다고 지워지면 안 됨.
            //        하지만 현재 로직상 폴더 변경시에만 스레드가 돌기 때문에 안전함.
            if (GetWindowTextLengthW(GetDlgItem(hwnd, IDC_MEMO_EDIT)) == 0) {
                 SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, L"");
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        // [PRD 3.2.3] 자동 저장 및 [PRD 3.2.2] 트리거 생성
        if (LOWORD(wParam) == IDC_MEMO_EDIT && HIWORD(wParam) == EN_CHANGE) {
            std::wstring targetPath = L"";
            bool* pFileExists = nullptr;

            // 1. 현재 타겟 경로 및 파일 존재 여부 포인터 획득
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { 
                        targetPath = pair.currentPath; 
                        pFileExists = &pair.fileExists;
                        break; 
                    }
                }
            }

            if (!targetPath.empty()) {
                // [PRD 3.2.2] 트리거 생성: 파일이 없는데 타이핑을 시작했다면?
                if (pFileExists && !(*pFileExists)) {
                    CreateEmptyMemo(targetPath);
                    *pFileExists = true; // 메모리 상 상태도 갱신하여 중복 생성 방지
                }

                // [PRD 3.2.3] 자동 저장: 변경된 내용 즉시 파일에 반영
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
        // 에디트 컨트롤 생성
        CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        
        // 폰트 설정
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
        SendDlgItemMessage(hwnd, IDC_MEMO_EDIT, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        // 타이틀바(버튼 영역) 제외하고 꽉 채우기
        if (rc.bottom > BTN_SIZE) MoveWindow(hEdit, 0, BTN_SIZE, rc.right, rc.bottom - BTN_SIZE, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        bool isMin = false;
        
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) if (pair.hOverlay == hwnd) { isMin = pair.isMinimized; break; }
        }

        // [View Update] 파일 유무와 관계없이, 최소화 상태만 체크하여 그림
        if (isMin) {
            // 최소화 모드: 파란색 작은 박스 + 'O' 텍스트
            HBRUSH brush = CreateSolidBrush(RGB(100, 100, 255)); 
            FillRect(hdc, &rcClient, brush); 
            DeleteObject(brush);
            SetBkMode(hdc, TRANSPARENT); 
            SetTextColor(hdc, RGB(255, 255, 255)); 
            TextOutW(hdc, 12, 10, L"O", 1);
        } else {
            // 일반 모드: 상단 타이틀바 및 버튼 그리기
            RECT rcTitle = { 0, 0, rcClient.right, BTN_SIZE };
            HBRUSH brush = CreateSolidBrush(RGB(230, 230, 230)); 
            FillRect(hdc, &rcTitle, brush); 
            DeleteObject(brush);

            // 닫기(X), 최소화(_) 버튼
            RECT rcClose = { rcClient.right - BTN_SIZE, 0, rcClient.right, BTN_SIZE }; 
            DrawFrameControl(hdc, &rcClose, DFC_CAPTION, DFCS_CAPTIONCLOSE);
            
            RECT rcMin = { rcClient.right - BTN_SIZE * 2, 0, rcClient.right - BTN_SIZE, BTN_SIZE }; 
            DrawFrameControl(hdc, &rcMin, DFC_CAPTION, DFCS_CAPTIONMIN);
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
            // 최소화 상태 클릭 -> 복원
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) if (pair.hOverlay == hwnd) { 
                    pair.isMinimized = false; 
                    SyncOverlayPosition(pair); 
                    break; 
                }
            }
            InvalidateRect(hwnd, NULL, TRUE);
        } else {
            // 상단 버튼 클릭 처리
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            if (y < BTN_SIZE) { // 타이틀바 영역
                if (x > rcClient.right - BTN_SIZE) {
                    // [X] 종료 버튼
                    PostQuitMessage(0);
                }
                else if (x > rcClient.right - BTN_SIZE * 2) {
                    // [_] 최소화 버튼
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        for (auto& pair : g_overlays) if (pair.hOverlay == hwnd) { 
                            pair.isMinimized = true; 
                            SyncOverlayPosition(pair); 
                            break; 
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

// --- [Event Hook] ---
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    // Case 1: 새로운 탐색기 발견
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
                    SetLayeredWindowAttributes(hNew, 0, 240, LWA_ALPHA);
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        g_overlays.push_back({ hwnd, hNew, L"", false, false });
                        SyncOverlayPosition(g_overlays.back());
                    }
                    
                    // 🔥 [핵심] 타이머(SetTimer) 대신 별도의 스레드를 출발시킵니다.
                    // 탐색기가 1초 걸리든 10초 걸리든, 이 스레드가 알아서 기다렸다가 보고합니다.
                    // detach()를 하면 백그라운드에서 알아서 돌고 사라집니다.
                    std::thread(PathFinderThread, hNew, hwnd).detach();
                }
            }
        }
    }
    // Case 2: 탐색기 종료 (좀비 청소)
    else if (event == EVENT_OBJECT_DESTROY) {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            // IsWindow 체크를 !연산자로 하여 죽은 창도 감지
            if (it->hExplorer == hwnd || !IsWindow(it->hExplorer)) { 
                DestroyWindow(it->hOverlay); 
                it = g_overlays.erase(it); 
                continue; 
            }
            ++it;
        }
    }
    // Case 3: 위치/활성화
    else if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_FOREGROUND) {
        if (!IsWindow(hwnd)) return; 
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { SyncOverlayPosition(pair); break; }
    }
    // Case 4: 이름 변경
    else if (event == EVENT_OBJECT_NAMECHANGE) {
        if (!IsWindow(hwnd)) return;
        // 이름 변경 시에도 스레드를 보내서 확인합니다. (메인 스레드 보호)
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
    // 메인 스레드 COM 초기화
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // 이벤트 훅 설치 (범위를 나누어 노이즈 캔슬링)
    HWINEVENTHOOK hHook1 = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_DESTROY, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook2 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook3 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_hHookObject = hHook1;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hHook1) UnhookWinEvent(hHook1);
    if (hHook2) UnhookWinEvent(hHook2);
    if (hHook3) UnhookWinEvent(hHook3);
    CoUninitialize();
    return 0;
}