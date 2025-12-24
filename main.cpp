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
#define WM_SAFE_CHECK (WM_USER + 999) // "안전하게 확인해줘" 라는 우리만의 신호

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

// 🔥 [추가] 닫기 애니메이션 감지를 위한 상수 정의
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif

namespace fs = std::filesystem;

// --- [상수 정의 업데이트] ---
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";
const int OVERLAY_WIDTH = 400;       
const int OVERLAY_HEIGHT = 600;      
const int EXPANDED_WIDTH = 600;      
const int EXPANDED_HEIGHT = 900;     
const int MINIMIZED_SIZE = 50;       // 🔥 [디자인] 최소화 크기 40 -> 50으로 변경
const int BTN_SIZE = 25;             
const int DEFAULT_FONT_SIZE = 22;    


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
// 🔥 [추가] 쿨다운 타이머 (이 시간까지는 위험한 작업 중지)
DWORD g_cooldownTime = 0;
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

// [Helper] 즉시 시각적 상태 변경 (애니메이션 락 회피용)
// -> OS의 Show/Hide 명령보다 투명도 조절이 훨씬 빠름 (GPU 처리)
void SetInstantVisibility(HWND hwnd, bool visible) {
    if (!visible) {
        // 1. 먼저 투명하게 만듦 (즉시 사라짐)
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        // 2. 그 다음 천천히 닫히라고 명령 (딜레이 생겨도 이미 눈엔 안 보임)
        ShowWindow(hwnd, SW_HIDE);
    } else {
        // 1. 투명도 복구
        SetLayeredWindowAttributes(hwnd, 0, 200, LWA_ALPHA); // 기존 투명도 200 유지
        // 2. 보이기
        ShowWindow(hwnd, SW_SHOWNA);
    }
}

// --- [핵심 함수 2] 위치 동기화 (Crash 방지 강화) ---
void SyncOverlayPosition(const OverlayPair& pair) {
    if (!IsWindow(pair.hExplorer)) return;

    // 🔥 [안전장치 1] 탐색기가 안 보이면(최소화/이동 중) 건드리지 않음
    //if (!IsWindowVisible(pair.hExplorer)) return;

    RECT rcExp;
    // 🔥 [안전장치 2] DWM 함수가 실패하면(창이 깨지는 중이면) 즉시 중단
    HRESULT hr = DwmGetWindowAttribute(pair.hExplorer, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExp, sizeof(rcExp));
    if (FAILED(hr)) {
        GetWindowRect(pair.hExplorer, &rcExp); // 백업: 일반 사각형 가져오기
    }

    bool smallMode = pair.isMinimized;
    int targetW = smallMode ? MINIMIZED_SIZE : (pair.isExpanded ? EXPANDED_WIDTH : OVERLAY_WIDTH);
    int targetH = smallMode ? MINIMIZED_SIZE : (pair.isExpanded ? EXPANDED_HEIGHT : OVERLAY_HEIGHT);

    int x = rcExp.right - targetW - 25;
    int y = rcExp.bottom - targetH - 25;

    SetWindowPos(pair.hOverlay, NULL, x, y, targetW, targetH, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
    
    if (hEdit) {
        if (smallMode) ShowWindow(hEdit, SW_HIDE);
        else ShowWindow(hEdit, SW_SHOW);
    }
    
    SetInstantVisibility(pair.hOverlay, true);
}

// [PRD 3.1.1] 비동기 경로 탐색 (Async Pathfinder)
// -> 메인 UI 멈춤 방지 및 파일 존재 여부 확인 후 보고
void PathFinderThread(HWND hOverlay, HWND hExplorer) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::wstring foundPath = L"";
    
    // [Retry Policy] 탐색기가 초기화될 때까지 최대 5번(1.5초) 시도
    for (int i = 0; i < 5; i++) {
        if (!IsWindow(hExplorer)) break;
        foundPath = GetExplorerPath(hExplorer);
        
        if (!foundPath.empty()) break; 
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // 결과 처리
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
                    // [PRD 4.2] 초기 상태 결정 로직을 여기서 제거하고 WindowProc으로 이관함 (Thread-Safety)
                    break;
                }
            }
        }
        // 메인 스레드에 "탐색 완료(파일 존재 여부)" 신호 전송
        PostMessage(hOverlay, WM_UPDATE_UI_FromThread, (WPARAM)exists, 0);
    }
    CoUninitialize();
}

// [Helper] 쿨다운 때문에 놓친 업데이트를 나중에 다시 수행
void DelayedUpdate(HWND hExplorer, int delayMs) {
    // 1. 지정된 시간만큼 대기 (쿨다운이 끝날 때까지)
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

    // 2. 윈도우가 여전히 살아있는지 확인
    if (!IsWindow(hExplorer)) return;

    // 3. 오버레이 찾기
    HWND hOverlay = NULL;
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hExplorer) {
                hOverlay = pair.hOverlay;
                break;
            }
        }
    }

    // 4. 경로 다시 탐색 요청 (놓친 업데이트 수행)
    if (hOverlay) {
        PathFinderThread(hOverlay, hExplorer);
    }
}

// [PRD 4.0] & [PRD 5.0] 메인 윈도우 프로시저
// -> UI 업데이트, 페인팅, 입력 처리를 담당
// [PRD 4.0] 메인 윈도우 프로시저 (Message Queue Edition)
// [PRD 4.0] 메인 윈도우 프로시저 (Thread Explosion Fix)
// [PRD 4.0] 메인 윈도우 프로시저 (Janitor Edition: 전체 청소)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    
    // 🔥 [핵심] 안전 확인 메시지 (온 김에 대청소)
    case WM_SAFE_CHECK: {
        DWORD eventType = (DWORD)lParam;
        bool needUpdatePath = false;
        HWND hTargetExplorer = NULL;

        std::lock_guard<std::mutex> lock(g_overlayMutex);
        
        // 🔥 [Fix] 특정 타겟만 찾는 게 아니라, 리스트 전체를 순회하며 '죽은 놈'은 전부 삭제
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            
            // 1. 탐색기 핸들이 유효한지 확인 (좀비 체크)
            if (!IsWindow(it->hExplorer)) {
                // 죽었으면 즉시 제거 (이게 남아있으면 나중에 탭 클릭 시 터짐)
                DestroyWindow(it->hOverlay);
                it = g_overlays.erase(it);
                continue; // 다음 놈으로 넘어감
            }

            // 2. 살아있는 녀석들에 대한 처리
            // 현재 메시지를 받은 오버레이(hwnd)라면 상태 동기화 수행
            if (it->hOverlay == hwnd) {
                hTargetExplorer = it->hExplorer;

                if (!IsWindowVisible(it->hExplorer)) {
                    SetInstantVisibility(hwnd, false);
                } else {
                    SyncOverlayPosition(*it);
                    // 탭 이동(이름 변경)이나 생성 이벤트면 경로 갱신 필요
                    if (eventType == EVENT_OBJECT_NAMECHANGE || eventType == EVENT_OBJECT_SHOW) {
                        needUpdatePath = true;
                    }
                }
            }
            // 메시지 대상이 아니더라도 살아있는 놈들은 놔둠
            ++it;
        }

        // 3. 경로 갱신이 필요하면 스레드 시작 (Lock 풀린 후 실행)
        if (needUpdatePath && hTargetExplorer && IsWindow(hTargetExplorer)) {
            std::thread(PathFinderThread, hwnd, hTargetExplorer).detach();
        }

        return 0;
    }

    // ... (이 아래 내용은 기존과 완벽히 동일합니다) ...
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
                    pair.isMinimized = !exists; 
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
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { targetPath = pair.currentPath; break; }
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
        bool isMin = false, isExp = false, hasFile = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) { 
                    isMin = pair.isMinimized; 
                    isExp = pair.isExpanded; 
                    hasFile = pair.fileExists; 
                    break; 
                }
            }
        }
        HBRUSH hBgBrush = CreateSolidBrush(BG_COLOR);
        FillRect(hdc, &rcClient, hBgBrush);
        DeleteObject(hBgBrush);

        if (isMin) {
            HFONT hIconFont = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hIconFont);
            SetBkMode(hdc, TRANSPARENT); 
            SetTextColor(hdc, RGB(50, 50, 50)); 
            RECT rcIcon = rcClient;
            DrawTextW(hdc, hasFile ? L"▤" : L"+", -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hOldFont);
            DeleteObject(hIconFont);
            HBRUSH hBorderBrush = CreateSolidBrush(RGB(100, 100, 100)); 
            FrameRect(hdc, &rcClient, hBorderBrush);
            DeleteObject(hBorderBrush);
        } else {
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0)); 
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
            int btnW = BTN_SIZE;
            int right = rcClient.right;
            MoveToEx(hdc, right - btnW + 8, 8, NULL); LineTo(hdc, right - 8, btnW - 8);
            MoveToEx(hdc, right - 8, 8, NULL); LineTo(hdc, right - btnW + 8, btnW - 8);
            int expRight = right - btnW;
            Rectangle(hdc, expRight - btnW + 8, 8, expRight - 8, btnW - 8);
            int minRight = expRight - btnW;
            MoveToEx(hdc, minRight - btnW + 8, btnW - 8, NULL); LineTo(hdc, minRight - 8, btnW - 8);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);
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
                for (auto& pair : g_overlays) {
                    if (pair.hOverlay == hwnd) { 
                        if (pair.currentPath.empty()) return 0;
                        if (!pair.fileExists) {
                            CreateEmptyMemo(pair.currentPath);
                            pair.fileExists = true; 
                        }
                        pair.isMinimized = false; 
                        SyncOverlayPosition(pair); 
                        break; 
                    }
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
    
    case WM_CTLCOLOREDIT: { 
        HDC hdcEdit = (HDC)wParam; 
        SetBkColor(hdcEdit, BG_COLOR); 
        SetTextColor(hdcEdit, RGB(0, 0, 0));
        static HBRUSH hEditBgBrush = CreateSolidBrush(BG_COLOR);
        return (LRESULT)hEditBgBrush; 
    }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
// [PRD 2.1.3] & [PRD 4.2.3] 윈도우 이벤트 훅 프로시저
// -> 탐색기 생성 감지, 숨김, 파괴, 그리고 'Cloaked(닫기 동작)'을 감지하여 오버레이 제어
// [PRD 2.1.3] & [PRD 4.2.3] 윈도우 이벤트 훅 프로시저 (Clean & Fast)
// [PRD 2.1.3] 윈도우 이벤트 훅 (Event ID 전달 추가)

// [PRD 2.1.3] & [PRD 4.2.3] 윈도우 이벤트 훅 프로시저 (Cooldown Edition)
// [PRD 2.1.3] & [PRD 4.2.3] 윈도우 이벤트 훅 프로시저 (Final Polish)
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    DWORD now = GetTickCount();

    // Case 1: 탐색기 창 생성
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
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_LAYERED, CLASS_NAME, L"Memo", WS_POPUP, 
                                           0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hNew) {
                    SetLayeredWindowAttributes(hNew, 0, 200, LWA_ALPHA);
                    UpdateMemoFont(GetDlgItem(hNew, IDC_MEMO_EDIT), DEFAULT_FONT_SIZE);
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        g_overlays.push_back({ hwnd, hNew, L"", false, false, false, DEFAULT_FONT_SIZE });
                    }
                    std::thread(PathFinderThread, hNew, hwnd).detach();
                }
            }
        }
    }
    // Case 2: 숨김/파괴/가려짐 (쿨다운 발동)
    else if (event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_CLOAKED) {
        
        // 위험 상황 감지 -> 0.5초 쿨다운 설정
        if (event == EVENT_OBJECT_DESTROY) {
            g_cooldownTime = now + 500; 
        }

        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            if (it->hExplorer == hwnd || !IsWindow(it->hExplorer)) {
                
                SetInstantVisibility(it->hOverlay, false); 
                
                if (event == EVENT_OBJECT_DESTROY || !IsWindow(it->hExplorer)) {
                    DestroyWindow(it->hOverlay); 
                    it = g_overlays.erase(it); 
                    continue; 
                }
            }
            ++it;
        }
    }
    // Case 3: 위치/포커스 변경
    else if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_FOREGROUND) {
        
        // 🔥 [수정] 쿨다운 중이면 -> 0.6초 뒤에 다시 확인해달라고 예약 (스레드 분리)
        if (now < g_cooldownTime) {
            std::thread(DelayedUpdate, hwnd, 600).detach();
            return;
        }

        if (!IsWindow(hwnd)) return; 

        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) { 
                SyncOverlayPosition(pair); 
                break; 
            }
        }
    }
    // Case 4: 이름 변경 (여기가 탭 병합 시 경로 갱신되는 시점)
    else if (event == EVENT_OBJECT_NAMECHANGE) {
        
        // 🔥 [수정] 쿨다운 중이면 -> 0.6초 뒤에 경로 다시 읽으라고 예약
        if (now < g_cooldownTime) {
            std::thread(DelayedUpdate, hwnd, 600).detach();
            return;
        }

        if (!IsWindow(hwnd)) return;
        if (!IsWindowVisible(hwnd)) return; 
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
    wc.hbrBackground = CreateSolidBrush(BG_COLOR); 
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // 기존 훅들
    HWINEVENTHOOK hHook1 = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook2 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK hHook3 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    
    // 🔥 [추가] 닫기 애니메이션(Cloaked) 감지용 훅
    HWINEVENTHOOK hHook4 = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED, NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

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
    if (hHook4) UnhookWinEvent(hHook4); // 🔥 [추가] 해제
    
    CoUninitialize();
    return 0;
}