// 👇 유니코드 설정 (반드시 맨 위)
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <vector>
#include <string>
#include <algorithm>

// 전역 변수: 현재 떠 있는 오버레이 윈도우들을 관리하는 리스트
struct OverlayPair {
    HWND hExplorer; // 타겟 탐색기 핸들
    HWND hOverlay;  // 내가 만든 오버레이 핸들
};
std::vector<OverlayPair> g_overlays;

// 오버레이 윈도우의 클래스 이름
const wchar_t CLASS_NAME[] = L"ExplorerMemoOverlayClass";

// 오버레이 윈도우 크기 설정
const int OVERLAY_WIDTH = 250;
const int OVERLAY_HEIGHT = 350;
const int PADDING_X = 20; // 우측 여백
const int PADDING_Y = 20; // 하단 여백

// 1. 오버레이 윈도우의 동작을 정의하는 함수 (WndProc)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        // 작동 확인을 위해 노란색 배경으로 칠함
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 200)); 
        FillRect(hdc, &ps.rcPaint, brush);
        DeleteObject(brush);

        // 테스트 텍스트 출력
        SetBkMode(hdc, TRANSPARENT);
        TextOutW(hdc, 10, 10, L"탐색기 추적 중...", 9);
        
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0; // 메인 프로그램은 죽지 않게 함
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

    return 0;
}