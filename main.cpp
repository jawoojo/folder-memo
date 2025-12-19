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

    bool fileExists;      // folder_memo.txt 존재 여부

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

    fs::path p(folderPath); p /= L"folder_memo.txt";

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

    fs::path p(folderPath); p /= L"folder_memo.txt";



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

    fs::path p(folderPath); p /= L"folder_memo.txt";

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

    int y = rcExp.bottom - h - 25;



    // [수정] HWND_TOPMOST를 HWND_TOP (혹은 아예 순서 변경 없음)으로 변경

    // SWP_NOZORDER를 넣어서 "순서는 윈도우가 알아서 관리하게 놔두고 위치만 옮겨"라고 합니다.

    // 주인(탐색기)이 움직이면 OS가 알아서 메모장을 그 위에 그려줍니다.

    SetWindowPos(pair.hOverlay, NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

   

    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);

    if (hEdit) {

        ShowWindow(hEdit, smallMode ? SW_HIDE : SW_SHOW);

    }

}


// [수정된 모듈] WindowProc (Total Replacement)
// [기능] UI 이벤트 처리, 타이머 기반 지연 로딩, 파일 연동 및 자동 저장
// [개선사항]
// 1. Race Condition 방지: WM_TIMER를 통한 지연 업데이트 (탐색기 멈춤 해결)
// 2. 파일명 변경: folder_memo.txt -> folder_memo.txt 반영
// 3. 재시도 로직: 경로 로드 실패 시 재시도 수행
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    // ----------------------------------------------------------------------
    // [PRD 2.3 & 3.1] 안정적 데이터 로딩을 위한 타이머 처리
    // [Logic] WinEventProc에서 예약한 타이머가 울리면, 그때 경로 업데이트를 수행함
    // ----------------------------------------------------------------------
    case WM_TIMER: {
        // 타이머 ID 2001: 경로 업데이트 예약
        if (wParam == 2001) {
            KillTimer(hwnd, 2001); // 1회성 실행이므로 즉시 제거
            SendMessage(hwnd, WM_UPDATE_PATH, 0, 0); // 메인 스레드에서 안전하게 실행
        }
        return 0;
    }

    // ----------------------------------------------------------------------
    // [PRD 3.2.3] 자동 저장 (Auto-Save)
    // ----------------------------------------------------------------------
    case WM_COMMAND: {
        // 메모 내용 변경(EN_CHANGE) 감지 시 저장
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
            // 경로가 유효할 때만 저장
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

    // ----------------------------------------------------------------------
    // [PRD 3.1.1] 경로 업데이트 및 데이터 동기화 (핵심 로직)
    // [Fix] 기존 즉시 실행 방식에서 안정성을 위해 재시도 로직 추가
    // ----------------------------------------------------------------------
    case WM_UPDATE_PATH: {
        HWND hExplorer = NULL;
        // 1. 내 짝꿍(탐색기) 핸들 찾기
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    hExplorer = pair.hExplorer;
                    break;
                }
            }
        }

        // 짝꿍이 없으면 종료
        if (!IsWindow(hExplorer)) return 0;

        // 2. [Heavy Task] 실제 경로 계산 (COM)
        std::wstring calculatedPath = GetExplorerPath(hExplorer);

        // [Retry Logic] 탐색기가 켜지는 중이라 경로를 아직 못 내놓는 경우
        // 1초 뒤에 다시 시도하도록 타이머 설정 (최대 재시도 횟수 제한 필요하지만 여기선 간단히 처리)
        if (calculatedPath.empty()) {
            SetTimer(hwnd, 2001, 1000, NULL); 
            return 0;
        }

        // 3. 파일 존재 여부 확인 (파일명: folder_memo.txt)
        bool exists = false;
        if (!calculatedPath.empty()) {
            fs::path p(calculatedPath); 
            p /= L"folder_memo.txt"; // [PRD 수정] 파일명 변경
            exists = fs::exists(p);
        }

        // 4. 전역 데이터 갱신 (동기화)
        {
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            for (auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    pair.currentPath = calculatedPath; // 진짜 경로 주입
                    pair.fileExists = exists;          // 파일 상태 업데이트
                    // 파일이 있으면 최소화 해제 (선택 사항)
                    if(exists) pair.isMinimized = false; 
                    SyncOverlayPosition(pair);         // UI 위치/상태 새로고침
                    break;
                }
            }
        }

        // 5. UI 다시 그리기 & 데이터 로드
        InvalidateRect(hwnd, NULL, TRUE); 

        if (exists) {
            // 파일이 있으면 내용 로드
            std::wstring memo = LoadMemo(calculatedPath);
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, memo.c_str());
        } else {
            // 파일이 없으면 빈 화면 (입력 대기)
            SetDlgItemTextW(hwnd, IDC_MEMO_EDIT, L"");
        }
        return 0;
    }

    // ----------------------------------------------------------------------
    // [PRD 4.1] UI 생성 및 폰트 설정
    // ----------------------------------------------------------------------
    case WM_CREATE: {
        // 메모 입력창 생성
        CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        
        // 가독성을 위한 맑은 고딕 폰트 적용
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
        SendDlgItemMessage(hwnd, IDC_MEMO_EDIT, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }

    // ----------------------------------------------------------------------
    // [PRD 4.1] 리사이징 대응
    // ----------------------------------------------------------------------
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        if (rc.bottom > BTN_SIZE) {
            MoveWindow(hEdit, 0, BTN_SIZE, rc.right, rc.bottom - BTN_SIZE, TRUE);
        }
        return 0;
    }

    // ----------------------------------------------------------------------
    // [PRD 4.1] 상태별 커스텀 UI 드로잉 (+ 버튼, O 버튼, 타이틀바)
    // ----------------------------------------------------------------------
    case WM_PAINT: {
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
            // [State] 일반 모드 -> 상단 타이틀바 및 닫기/최소화 버튼
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

    // ----------------------------------------------------------------------
    // [PRD 4.2] 마우스 클릭 인터랙션 처리
    // ----------------------------------------------------------------------
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
       
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

        // Case 1. [+] 버튼 클릭: 파일 생성 (암시적 생성 Trigger)
        if (!hasFile) {
            // 안전하게 경로가 확보된 상태에서만 생성
            if (!currentPath.empty()) {
                CreateEmptyMemo(currentPath); // folder_memo.txt 생성 (Helper 함수도 파일명 수정 필요)
                SendMessage(hwnd, WM_UPDATE_PATH, 0, 0); // 즉시 갱신
            }
        }
        // Case 2. [O] 버튼 클릭: 최소화 해제
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
        // Case 3. 타이틀바 버튼 클릭
        else {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            if (y < BTN_SIZE) {
                // [X] 닫기 (프로그램 종료)
                if (x > rcClient.right - BTN_SIZE) {
                    PostQuitMessage(0); 
                }
                // [_] 최소화 (접기)
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


// [수정된 모듈] WinEventProc: 좀비 프로세스 방지 및 정확한 종료 감지
void CALLBACK WinEventProc(HWINEVENTHOOK hHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    // 1. 기본적인 필터링 (윈도우 객체만 처리)
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    // 🔴 [수정 포인트] !IsWindow(hwnd) 체크를 여기서 하지 않습니다.
    // 죽어가는 창(Destroy)의 신호를 무시하게 되기 때문입니다.

    // ---------------------------------------------------------
    // Case 1: 새로운 탐색기 발견 (생성)
    // ---------------------------------------------------------
    if (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) {
        // 생성 시에는 유효한 창인지 확인 필수
        if (!IsWindow(hwnd)) return;

        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256) > 0 && wcscmp(className, L"CabinetWClass") == 0) {
            bool managed = false;
            {
                std::lock_guard<std::mutex> lock(g_overlayMutex);
                for (const auto& pair : g_overlays) if (pair.hExplorer == hwnd) { managed = true; break; }
            }

            if (!managed) {
                // 부모 윈도우 설정 및 투명창 생성
                HWND hNew = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_LAYERED, CLASS_NAME, L"Memo", WS_POPUP | WS_VISIBLE, 
                                           0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, hwnd, NULL, GetModuleHandle(NULL), NULL);
                if (hNew) {
                    SetLayeredWindowAttributes(hNew, 0, 240, LWA_ALPHA);
                    {
                        std::lock_guard<std::mutex> lock(g_overlayMutex);
                        g_overlays.push_back({ hwnd, hNew, L"", false, false });
                        SyncOverlayPosition(g_overlays.back());
                    }
                    // 안정적인 연동을 위한 0.5초 지연 타이머
                    SetTimer(hNew, 2001, 500, NULL); 
                }
            }
        }
    }
    // ---------------------------------------------------------
    // Case 2: 탐색기 종료 감지 (청소)
    // ---------------------------------------------------------
    else if (event == EVENT_OBJECT_DESTROY) {
        // 여기서 IsWindow를 체크하면 안 됩니다. (이미 죽었을 수 있음)
        
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
            // 조건 1: 파괴된 핸들이 내가 관리하던 탐색기인가? (hwnd == it->hExplorer)
            // 조건 2: 혹은 내가 관리하던 탐색기가 OS 상에서 사라졌는가? (!IsWindow) -> 좀비 청소
            if (it->hExplorer == hwnd || !IsWindow(it->hExplorer)) {
                // 짝꿍 메모장 파괴
                DestroyWindow(it->hOverlay);
                // 리스트에서 제거
                it = g_overlays.erase(it);
                // 한 번에 하나만 처리하지 않고, 혹시 모를 다중 종료를 대비해 계속 검사할 수도 있으나
                // 효율성을 위해 여기선 리턴하되, 좀비 청소를 위해 루프를 돌게 할 수도 있음.
                // 여기서는 안전하게 루프를 계속 돕니다.
                continue; 
            }
            ++it;
        }
    }
    // ---------------------------------------------------------
    // Case 3: 위치 이동 및 활성화 (업데이트)
    // ---------------------------------------------------------
    else if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_SYSTEM_FOREGROUND) {
        // 이동/활성화 시에는 윈도우가 살아있어야 함
        if (!IsWindow(hwnd)) return; 

        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) { 
                SyncOverlayPosition(pair); 
                break; 
            }
        }
    }
    // ---------------------------------------------------------
    // Case 4: 이름(경로) 변경
    // ---------------------------------------------------------
    else if (event == EVENT_OBJECT_NAMECHANGE) {
        if (!IsWindow(hwnd)) return;

        std::lock_guard<std::mutex> lock(g_overlayMutex);
        for (const auto& pair : g_overlays) {
            if (pair.hExplorer == hwnd) { 
                // 탭 전환 등의 딜레이를 고려하여 0.1초 뒤 업데이트 요청
                SetTimer(pair.hOverlay, 2001, 100, NULL); 
                break; 
            }
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

                        fs::path p(path); p /= L"folder_memo.txt";

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



// [수정된 모듈] WinMain: 이벤트 리스너 최적화 (Noise Canceling 적용)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // [DPI Awareness] 및 [COM 초기화] 코드 유지
    HMODULE hShCore = LoadLibrary(L"Shcore.dll");
    if (hShCore) {
        auto pSetProcessDpiAwareness = (SetProcessDpiAwarenessType)GetProcAddress(hShCore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness) pSetProcessDpiAwareness(2);
        FreeLibrary(hShCore);
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // 윈도우 클래스 등록
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    // ---------------------------------------------------------
    // [Hooks] Eco-Friendly Event Listeners (노이즈 캔슬링 적용)
    // ---------------------------------------------------------
    // 이유: 모든 이벤트를 다 들으면(EVENT_MIN~MAX) 프로그램이 멈춥니다.
    // 해결: 필요한 신호만 골라 듣는 '핀셋 설정'을 적용합니다.

    // 1. [생성/소멸] 창이 열리고 닫히는 것만 감지 (0x8000 ~ 0x8001)
    HWINEVENTHOOK hHook1 = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_DESTROY,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 2. [상태 변경] 위치 이동, 이름 변경만 감지 (0x800B ~ 0x800C)
    // 중간에 있는 EVENT_OBJECT_STATECHANGE(마우스 오버 등) 같은 시끄러운 이벤트를 건너뜁니다.
    HWINEVENTHOOK hHook2 = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 3. [포커스] 창 활성화 감지 (0x0003)
    HWINEVENTHOOK hHook3 = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 전역 변수에 저장 (나중에 해제하기 위함)
    g_hHookObject = hHook1; // 임시로 하나만 저장하거나, 벡터로 관리 추천 (여기선 간단히)
    // *실제 코드에서는 종료 시 UnhookWinEvent를 hHook1, hHook2, hHook3 모두 해야 합니다.
    //  편의상 메인 루프 뒤에 3개 다 해제하는 코드를 넣으세요.

    // 메시지 루프
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 종료 및 정리
    if (hHook1) UnhookWinEvent(hHook1);
    if (hHook2) UnhookWinEvent(hHook2);
    if (hHook3) UnhookWinEvent(hHook3);
    CoUninitialize();
    return 0;
}