// 👇 유니코드 설정 (반드시 맨 위)
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <codecvt>
#include <UIAutomation.h>
#include <comdef.h>

// 라이브러리 링크 (MinGW/Visual Studio 호환)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

// 전역 변수
IUIAutomation* g_pAutomation = NULL;

// 메모 파일 입출력 함수
std::wstring LoadMemo(const std::wstring& folderPath) {
    std::wstring filePath = folderPath + L"\\system_memo.txt";
    // std::ifstream file(filePath); // 이 줄 삭제 (컴파일 에러 원인)
    
    // MinGW/Standard C++에서 유니코드 경로 파일 열기
    #ifdef _MSC_VER
        std::wifstream wif(filePath);
        wif.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t>)); // UTF-8 가정
    #else
        // MinGW 등의 경우
        std::ifstream wif(std::string(filePath.begin(), filePath.end())); // 간단히 변환 시도 (한글 경로 깨질 수 있음)
        // 실제로는 Windows API CreateFile을 쓰는게 가장 확실함
    #endif

    // 안전하게 Windows API 사용 (경로 문제 해결)
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0) {
        CloseHandle(hFile);
        return L"";
    }

    std::vector<char> buffer(fileSize + 1);
    DWORD bytesRead;
    ReadFile(hFile, buffer.data(), fileSize, &bytesRead, NULL);
    buffer[bytesRead] = '\0';
    CloseHandle(hFile);

    // MultiByteToWideChar (UTF-8 -> WCHAR)
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

    // WCHAR -> UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, NULL, 0, NULL, NULL);
    if (len > 0) {
        std::vector<char> buf(len);
        WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, buf.data(), len, NULL, NULL);
        DWORD bytesWritten;
        WriteFile(hFile, buf.data(), len - 1, &bytesWritten, NULL); // null terminator 제외
    }
    CloseHandle(hFile);
}

// 전역 변수: 현재 떠 있는 오버레이 윈도우들을 관리하는 리스트

// 전역 변수: 현재 떠 있는 오버레이 윈도우들을 관리하는 리스트
struct OverlayPair {
    HWND hExplorer; // 타겟 탐색기 핸들
    HWND hOverlay;  // 내가 만든 오버레이 핸들
    std::wstring currentPath; // 현재 보고 있는 경로
};
std::vector<OverlayPair> g_overlays;

// 오버레이 윈도우의 클래스 이름
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";

// 오버레이 윈도우 크기 설정
const int OVERLAY_WIDTH = 250;
const int OVERLAY_HEIGHT = 350;
const int PADDING_X = 20; // 우측 여백
const int PADDING_Y = 20; // 하단 여백

// 컨트롤 ID
#define IDC_MEMO_EDIT 101

// 1. 오버레이 윈도우의 동작을 정의하는 함수 (WndProc)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDC_MEMO_EDIT && HIWORD(wParam) == EN_CHANGE) {
            // 내용 변경 시 저장 (너무 빈번하면 성능 이슈, 나중에 Timer로 최적화 필요)
            // 우선은 현재 오버레이의 경로를 찾아야 함
            for (const auto& pair : g_overlays) {
                if (pair.hOverlay == hwnd) {
                    if (!pair.currentPath.empty()) {
                        int len = GetWindowTextLengthW((HWND)lParam);
                        if (len >= 0) {
                            std::vector<wchar_t> buf(len + 1);
                            GetWindowTextW((HWND)lParam, buf.data(), len + 1);
                            SaveMemo(pair.currentPath, std::wstring(buf.data()));
                        }
                    }
                    break;
                }
            }
        }
        return 0;
    }
    case WM_CREATE: {
        // 에디터 컨트롤 생성
        CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
            0, 0, 0, 0, // 나중에 WM_SIZE에서 크기 조정
            hwnd, (HMENU)IDC_MEMO_EDIT, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);
        
        // 폰트 설정 (맑은 고딕, 9pt 정도)
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Malgun Gothic");
        SendDlgItemMessage(hwnd, IDC_MEMO_EDIT, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_SIZE: {
        // 윈도우 크기가 변하면 에디터도 꽉 차게 조정 (상단에 접기 버튼 공간 정도는 남길 수 있음)
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        
        // 에디터 위치: (0, 30) ~ (Width, Height)  -> 상단 30px는 타이틀바/버튼 영역
        HWND hEdit = GetDlgItem(hwnd, IDC_MEMO_EDIT);
        MoveWindow(hEdit, 0, 30, rcClient.right, rcClient.bottom - 30, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        // 배경을 살짝 회색으로
        HBRUSH brush = CreateSolidBrush(RGB(240, 240, 240)); 
        FillRect(hdc, &ps.rcPaint, brush);
        DeleteObject(brush);

        // 상단 타이틀바 영역 (경로 표시)
        SetBkMode(hdc, TRANSPARENT);
        
        std::wstring msg = L"";
        for (const auto& pair : g_overlays) {
            if (pair.hOverlay == hwnd) {
                if (!pair.currentPath.empty()) {
                    msg = pair.currentPath;
                }
                break;
            }
        }
        
        // 경로가 너무 길면 잘라서 표시하거나... 일단 그냥 출력
        TextOutW(hdc, 5, 5, msg.c_str(), msg.length());
        
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        // 에디터 배경색을 흰색으로 유지
        HDC hdcEdit = (HDC)wParam;
        SetBkColor(hdcEdit, RGB(255, 255, 255));
        SetTextColor(hdcEdit, RGB(0, 0, 0));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_DESTROY: // 오버레이 하나가 닫힐 때... 메인 루프는 종료하면 안됨
        return 0; 
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// 2. 현재 실행 중인 탐색기 창들을 찾는 콜백 함수
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    wchar_t className[256];
    GetClassNameW(hwnd, className, 256);

    // 탐색기의 클래스 이름은 보통 "CabinetWClass" 입니다.
    if (wcscmp(className, L"CabinetWClass") == 0) {
        std::vector<HWND>* explorers = (std::vector<HWND>*)lParam;
        explorers->push_back(hwnd);
    }
    return TRUE;
}

// UI Automation을 사용하여 탐색기 경로 가져오기
std::wstring GetExplorerPath(HWND hExplorer) {
    if (!g_pAutomation) return L"";

    IUIAutomationElement* pElement = NULL;
    if (FAILED(g_pAutomation->ElementFromHandle(hExplorer, &pElement)) || !pElement) {
        return L"";
    }

    // 조건: Edit Control 타입 (주소창이 보통 Edit Control임)
    IUIAutomationCondition* pCondition = NULL;
    VARIANT varProp;
    varProp.vt = VT_I4;
    varProp.lVal = UIA_EditControlTypeId;
    
    if (FAILED(g_pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, varProp, &pCondition))) {
        pElement->Release();
        return L"";
    }

    // 탐색기 내에서 Edit Control 찾기 (주소창)
    // 주의: 탐색기 구조에 따라 여러 개가 있을 수 있음. 보통 첫 번째가 주소창이거나, AutomationId가 '41477'임.
    // 여기서는 간단히 첫 번째 Edit Control의 Value를 가져와봄.
    IUIAutomationElement* pFound = NULL;
    pElement->FindFirst(TreeScope_Descendants, pCondition, &pFound);
    
    std::wstring result = L"";

    if (pFound) {
        // ValuePattern으로 값 가져오기
        IUIAutomationValuePattern* pValuePattern = NULL;
        if (SUCCEEDED(pFound->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern)) && pValuePattern) {
            BSTR bstrValue;
            if (SUCCEEDED(pValuePattern->get_CurrentValue(&bstrValue))) {
                if (bstrValue) {
                    result = bstrValue;
                    SysFreeString(bstrValue);
                }
            }
            pValuePattern->Release();
        }
        pFound->Release();
    }

    pCondition->Release();
    pElement->Release();

    return result;
}

// 3. 핵심 로직: 탐색기 위치를 계산해서 오버레이를 이동시킴
void UpdateOverlays(HINSTANCE hInstance) {
    // A. 현재 실행 중인 모든 탐색기 핸들 수집
    std::vector<HWND> currentExplorers;
    EnumWindows(EnumWindowsProc, (LPARAM)&currentExplorers);

    // B. 사라진 탐색기에 붙어있던 오버레이 제거
    for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
        if (!IsWindow(it->hExplorer)) { 
            DestroyWindow(it->hOverlay); // 탐색기가 꺼지면 오버레이도 삭제
            it = g_overlays.erase(it);
        } else {
            ++it;
        }
    }

    // C. 탐색기 목록을 순회하며 오버레이 관리
    for (HWND hExp : currentExplorers) {
        // 이미 이 탐색기에 오버레이가 붙어있는지 확인
        bool exists = false;
        for (auto& pair : g_overlays) {
            if (pair.hExplorer == hExp) {
                exists = true;
                
                // [위치 동기화 로직]
                RECT rc;
                GetWindowRect(hExp, &rc); // 탐색기 위치 가져오기

                // 경로 가져오기 테스트
                std::wstring path = GetExplorerPath(hExp);
                
                // 경로가 변경되었을 때만 처리
                if (!path.empty() && path != pair.currentPath) {
                    pair.currentPath = path;
                    
                    // 파일 읽어오기
                    std::wstring memo = LoadMemo(path);
                    
                    // 에디터에 내용 설정
                    HWND hEdit = GetDlgItem(pair.hOverlay, IDC_MEMO_EDIT);
                    if (hEdit) {
                        SetWindowTextW(hEdit, memo.c_str());
                    }

                    InvalidateRect(pair.hOverlay, NULL, TRUE); // 화면 갱신 요청
                }

                // 오버레이가 위치할 좌표 계산 (탐색기 우측 하단)
                int x = rc.right - OVERLAY_WIDTH - PADDING_X;
                int y = rc.bottom - OVERLAY_HEIGHT - PADDING_Y;

                // 탐색기가 최소화 상태인지 확인 (최소화면 숨김)
                if (IsIconic(hExp)) {
                    ShowWindow(pair.hOverlay, SW_HIDE);
                } else {
                    // 위치 이동 및 표시 (SWP_NOZORDER로 Z순서 유지)
                    SetWindowPos(pair.hOverlay, HWND_TOPMOST, x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
                break;
            }
        }

        // 오버레이가 없으면 새로 생성
        if (!exists) {
            HWND hNewOverlay = CreateWindowEx(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST, // 작업표시줄에 안 뜨게 | 최상위
                CLASS_NAME,
                L"MemoOverlay", // 윈도우 이름
                WS_POPUP | WS_BORDER, // 타이틀바 없는 팝업 스타일
                0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT,
                NULL, NULL, hInstance, NULL
            );

            if (hNewOverlay) {
                ShowWindow(hNewOverlay, SW_SHOW);
                g_overlays.push_back({ hExp, hNewOverlay });
            }
        }
    }
}

// 👇 기존 wWinMain 대신 이 함수로 전체를 교체하세요
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 0. COM 초기화
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        return 1;
    }

    // UI Automation 초기화
    hr = CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&g_pAutomation);
    if (FAILED(hr) || g_pAutomation == NULL) {
        CoUninitialize();
        return 1;
    }

    // 1. 윈도우 클래스 등록
    WNDCLASSW wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    RegisterClassW(&wc);

    // 2. 메시지 루프
    MSG msg = { };
    
    // 10ms마다 WM_TIMER 메시지 발생
    SetTimer(NULL, 1, 10, NULL); 

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            UpdateOverlays(hInstance);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // 정리
    if (g_pAutomation) g_pAutomation->Release();
    CoUninitialize();

    return 0;
}