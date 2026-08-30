#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kAppName[] = L"眺望 · EyeRest";
constexpr wchar_t kMainClass[] = L"EyeRest.MessageWindow";
constexpr wchar_t kBannerClass[] = L"EyeRest.ReminderBanner";
constexpr wchar_t kRestClass[] = L"EyeRest.RestOverlay";
constexpr wchar_t kSettingsClass[] = L"EyeRest.Settings";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kActivityTimer = 1;
constexpr UINT_PTR kBannerTimer = 2;
constexpr UINT_PTR kRestTimer = 3;
constexpr UINT kTrayId = 1;
constexpr DWORD kActivityPollMs = 5000;
constexpr ULONGLONG kIdleThresholdMs = 5ULL * 60ULL * 1000ULL;
constexpr int kDefaultWorkMinutes = 20;
constexpr int kRestSeconds = 20;

enum CommandId : int {
    CmdSettings = 1001,
    CmdPause = 1002,
    CmdRestNow = 1003,
    CmdStrict = 1004,
    CmdAutoStart = 1005,
    CmdExit = 1006,
    CmdStartRest = 1101,
    CmdSnooze = 1102,
    CmdSkip = 1103,
    CmdSaveSettings = 1201,
    CmdCancelSettings = 1202,
    CtlWorkMinutes = 1210,
    CtlStrict = 1211,
    CtlAutoStart = 1212,
};

struct Settings {
    int workMinutes = kDefaultWorkMinutes;
    bool strict = false;
    bool autoStart = false;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND mainWindow = nullptr;
    HWND bannerWindow = nullptr;
    HWND restWindow = nullptr;
    HWND settingsWindow = nullptr;
    HICON trayIcon = nullptr;
    HFONT uiFont = nullptr;
    HFONT titleFont = nullptr;
    Settings settings;
    std::wstring configPath;
    ULONGLONG activeMs = 0;
    ULONGLONG lastTick = 0;
    ULONGLONG pausedUntil = 0;
    ULONGLONG restEndsAt = 0;
    bool sessionLocked = false;
    bool exiting = false;
};

AppState g;
UINT g_taskbarCreated = 0;

LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK BannerWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK RestWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);

std::wstring GetExecutablePath() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::wstring(buffer.data(), length);
}

std::wstring GetConfigPath() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        return L"EyeRest.ini";
    }

    std::wstring directory = std::wstring(localAppData) + L"\\EyeRest";
    CoTaskMemFree(localAppData);
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\settings.ini";
}

void LoadSettings() {
    g.configPath = GetConfigPath();
    g.settings.workMinutes = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"General", L"WorkMinutes", kDefaultWorkMinutes, g.configPath.c_str())),
        1, 180);
    g.settings.strict = GetPrivateProfileIntW(L"General", L"Strict", 0, g.configPath.c_str()) != 0;
    g.settings.autoStart = GetPrivateProfileIntW(L"General", L"AutoStart", 0, g.configPath.c_str()) != 0;
}

void SaveSettings() {
    const std::wstring minutes = std::to_wstring(g.settings.workMinutes);
    WritePrivateProfileStringW(L"General", L"WorkMinutes", minutes.c_str(), g.configPath.c_str());
    WritePrivateProfileStringW(L"General", L"Strict", g.settings.strict ? L"1" : L"0", g.configPath.c_str());
    WritePrivateProfileStringW(L"General", L"AutoStart", g.settings.autoStart ? L"1" : L"0", g.configPath.c_str());
}

bool SetAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enable) {
        const std::wstring command = L"\"" + GetExecutablePath() + L"\" --background";
        result = RegSetValueExW(key, L"EyeRest", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"EyeRest");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void SetControlFont(HWND control, HFONT font = nullptr) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g.uiFont), TRUE);
}

COLORREF AccentColor() { return RGB(68, 145, 122); }

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

bool IsUserActive() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return true;
    const ULONGLONG now = GetTickCount64();
    const DWORD current32 = static_cast<DWORD>(now & 0xffffffffULL);
    const DWORD idle32 = current32 - info.dwTime;
    return static_cast<ULONGLONG>(idle32) < kIdleThresholdMs;
}

bool IsForegroundFullscreen() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == g.bannerWindow || foreground == g.restWindow ||
        foreground == g.settingsWindow || foreground == GetShellWindow()) {
        return false;
    }
    // A regular maximized window is not treated as presentation/game mode.
    if (IsZoomed(foreground)) return false;

    RECT windowRect{};
    if (!GetWindowRect(foreground, &windowRect)) return false;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &monitor)) return false;

    constexpr int tolerance = 2;
    return windowRect.left <= monitor.rcMonitor.left + tolerance &&
           windowRect.top <= monitor.rcMonitor.top + tolerance &&
           windowRect.right >= monitor.rcMonitor.right - tolerance &&
           windowRect.bottom >= monitor.rcMonitor.bottom - tolerance;
}

RECT ActiveMonitorRect(bool workArea) {
    HWND reference = GetForegroundWindow();
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST), &monitor);
    return workArea ? monitor.rcWork : monitor.rcMonitor;
}

void UpdateTrayTooltip() {
    if (!g.mainWindow) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g.mainWindow;
    data.uID = kTrayId;
    data.uFlags = NIF_TIP;

    std::wstring text;
    const ULONGLONG now = GetTickCount64();
    if (g.pausedUntil > now) {
        const ULONGLONG minutes = (g.pausedUntil - now + 59999) / 60000;
        text = L"眺望：已暂停，约 " + std::to_wstring(minutes) + L" 分钟后继续";
    } else {
        const ULONGLONG target = static_cast<ULONGLONG>(g.settings.workMinutes) * 60000ULL;
        const ULONGLONG remaining = target > g.activeMs ? target - g.activeMs : 0;
        const ULONGLONG minutes = (remaining + 59999) / 60000;
        text = L"眺望：约 " + std::to_wstring(minutes) + L" 分钟后休息";
    }
    wcsncpy_s(data.szTip, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g.mainWindow;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g.trayIcon;
    wcscpy_s(data.szTip, L"眺望 · EyeRest");
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    UpdateTrayTooltip();
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g.mainWindow;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void CloseReminderWindows() {
    if (g.bannerWindow) DestroyWindow(g.bannerWindow);
    if (g.restWindow) DestroyWindow(g.restWindow);
}

void ResetWorkCycle() {
    g.activeMs = 0;
    g.lastTick = GetTickCount64();
    UpdateTrayTooltip();
}

void FinishRest() {
    if (g.restWindow) DestroyWindow(g.restWindow);
    ResetWorkCycle();
}

void ShowRestOverlay() {
    if (g.restWindow) return;
    if (g.bannerWindow) DestroyWindow(g.bannerWindow);

    const RECT area = ActiveMonitorRect(false);
    g.restEndsAt = GetTickCount64() + static_cast<ULONGLONG>(kRestSeconds) * 1000ULL;
    g.restWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kRestClass, kAppName, WS_POPUP,
        area.left, area.top, area.right - area.left, area.bottom - area.top,
        nullptr, nullptr, g.instance, nullptr);
    if (!g.restWindow) return;

    SetLayeredWindowAttributes(g.restWindow, 0, 242, LWA_ALPHA);
    ShowWindow(g.restWindow, SW_SHOW);
    SetForegroundWindow(g.restWindow);
}

void ShowReminderBanner() {
    if (g.bannerWindow || g.restWindow) return;
    if (g.settings.strict) {
        ShowRestOverlay();
        return;
    }

    const RECT work = ActiveMonitorRect(true);
    constexpr int width = 430;
    constexpr int height = 190;
    const int x = work.right - width - 20;
    const int y = work.bottom - height - 20;
    g.bannerWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kBannerClass, kAppName, WS_POPUP,
        x, y, width, height,
        nullptr, nullptr, g.instance, nullptr);
    if (g.bannerWindow) {
        ShowWindow(g.bannerWindow, SW_SHOWNOACTIVATE);
        SetWindowPos(g.bannerWindow, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void SnoozeFiveMinutes() {
    if (g.bannerWindow) DestroyWindow(g.bannerWindow);
    const ULONGLONG target = static_cast<ULONGLONG>(g.settings.workMinutes) * 60000ULL;
    g.activeMs = target > 5ULL * 60000ULL ? target - 5ULL * 60000ULL : 0;
    g.lastTick = GetTickCount64();
    UpdateTrayTooltip();
}

void PauseOrResume() {
    const ULONGLONG now = GetTickCount64();
    if (g.pausedUntil > now) {
        g.pausedUntil = 0;
        g.lastTick = now;
    } else {
        CloseReminderWindows();
        g.pausedUntil = now + 60ULL * 60ULL * 1000ULL;
    }
    UpdateTrayTooltip();
}

void ActivityTick() {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG elapsed = now - g.lastTick;
    g.lastTick = now;
    elapsed = std::min<ULONGLONG>(elapsed, kActivityPollMs * 2ULL);

    if (g.pausedUntil != 0 && now >= g.pausedUntil) g.pausedUntil = 0;
    if (g.pausedUntil > now || g.sessionLocked || g.bannerWindow || g.restWindow || g.settingsWindow) {
        return;
    }
    if (IsUserActive()) g.activeMs += elapsed;

    const ULONGLONG target = static_cast<ULONGLONG>(g.settings.workMinutes) * 60000ULL;
    if (g.activeMs >= target && !IsForegroundFullscreen()) ShowReminderBanner();
    UpdateTrayTooltip();
}

void ShowTrayMenu(HWND owner) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const bool paused = g.pausedUntil > GetTickCount64();
    AppendMenuW(menu, MF_STRING, CmdSettings, L"设置…");
    AppendMenuW(menu, MF_STRING, CmdRestNow, L"现在休息 20 秒");
    AppendMenuW(menu, MF_STRING, CmdPause, paused ? L"继续计时" : L"暂停 1 小时");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g.settings.strict ? MF_CHECKED : 0), CmdStrict, L"严格模式");
    AppendMenuW(menu, MF_STRING | (g.settings.autoStart ? MF_CHECKED : 0), CmdAutoStart, L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdExit, L"退出眺望");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   point.x, point.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ShowSettingsWindow() {
    if (g.settingsWindow) {
        ShowWindow(g.settingsWindow, SW_RESTORE);
        SetForegroundWindow(g.settingsWindow);
        return;
    }

    const RECT work = ActiveMonitorRect(true);
    constexpr int width = 470;
    constexpr int height = 330;
    g.settingsWindow = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kSettingsClass, L"眺望设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        work.left + ((work.right - work.left) - width) / 2,
        work.top + ((work.bottom - work.top) - height) / 2,
        width, height, nullptr, nullptr, g.instance, nullptr);
    if (g.settingsWindow) {
        ShowWindow(g.settingsWindow, SW_SHOW);
        SetForegroundWindow(g.settingsWindow);
    }
}

void HandleCommand(HWND owner, int command) {
    switch (command) {
        case CmdSettings: ShowSettingsWindow(); break;
        case CmdPause: PauseOrResume(); break;
        case CmdRestNow: ShowRestOverlay(); break;
        case CmdStrict:
            g.settings.strict = !g.settings.strict;
            SaveSettings();
            break;
        case CmdAutoStart: {
            const bool desired = !g.settings.autoStart;
            if (SetAutoStart(desired)) {
                g.settings.autoStart = desired;
                SaveSettings();
            } else {
                MessageBoxW(owner, L"无法更新开机启动设置，请检查当前账户权限。", kAppName, MB_OK | MB_ICONWARNING);
            }
            break;
        }
        case CmdExit:
            g.exiting = true;
            DestroyWindow(g.mainWindow);
            break;
    }
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int width, int height) {
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  x, y, width, height, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.instance, nullptr);
    SetControlFont(button);
    return button;
}

LRESULT CALLBACK MainWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreated) {
        AddTrayIcon();
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
            SetTimer(window, kActivityTimer, kActivityPollMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == kActivityTimer) ActivityTick();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) g.sessionLocked = true;
            if (wParam == WTS_SESSION_UNLOCK) {
                g.sessionLocked = false;
                g.lastTick = GetTickCount64();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                g.lastTick = GetTickCount64();
            }
            return TRUE;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) ShowTrayMenu(window);
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) ShowSettingsWindow();
            return 0;
        case WM_COMMAND:
            HandleCommand(window, LOWORD(wParam));
            return 0;
        case WM_DESTROY:
            KillTimer(window, kActivityTimer);
            WTSUnRegisterSessionNotification(window);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK BannerWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateButton(window, CmdStartRest, L"开始休息", 24, 130, 118, 36);
            CreateButton(window, CmdSnooze, L"5 分钟后", 154, 130, 118, 36);
            CreateButton(window, CmdSkip, L"跳过本次", 284, 130, 118, 36);
            SetTimer(window, kBannerTimer, 12000, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == kBannerTimer) ShowRestOverlay();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case CmdStartRest: ShowRestOverlay(); break;
                case CmdSnooze: SnoozeFiveMinutes(); break;
                case CmdSkip:
                    DestroyWindow(window);
                    ResetWorkCycle();
                    break;
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillSolid(dc, client, RGB(247, 250, 249));
            RECT accent{0, 0, 7, client.bottom};
            FillSolid(dc, accent, AccentColor());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(30, 49, 44));
            HFONT old = static_cast<HFONT>(SelectObject(dc, g.titleFont));
            RECT title{24, 22, 405, 58};
            DrawTextW(dc, L"该让眼睛休息一下了", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, g.uiFont);
            SetTextColor(dc, RGB(83, 99, 94));
            RECT body{24, 68, 405, 112};
            DrawTextW(dc, L"看看约 6 米以外的地方，放松 20 秒。", -1, &body, DT_LEFT | DT_WORDBREAK);
            SelectObject(dc, old);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kBannerTimer);
            if (g.bannerWindow == window) g.bannerWindow = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK RestWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateButton(window, CmdSkip, L"提前结束  Esc", 0, 0, 150, 36);
            SetTimer(window, kRestTimer, 250, nullptr);
            return 0;
        case WM_SIZE: {
            HWND button = GetDlgItem(window, CmdSkip);
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            MoveWindow(button, (width - 150) / 2, height / 2 + 125, 150, 36, TRUE);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kRestTimer) {
                if (GetTickCount64() >= g.restEndsAt) FinishRest();
                else InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == CmdSkip) FinishRest();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) FinishRest();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillSolid(dc, client, RGB(18, 31, 28));
            SetBkMode(dc, TRANSPARENT);

            const ULONGLONG now = GetTickCount64();
            const int seconds = static_cast<int>((g.restEndsAt > now ? g.restEndsAt - now : 0) + 999) / 1000;
            const int centerX = client.right / 2;
            const int centerY = client.bottom / 2 - 35;
            RECT circle{centerX - 86, centerY - 86, centerX + 86, centerY + 86};
            HPEN basePen = CreatePen(PS_SOLID, 8, RGB(50, 70, 64));
            HPEN oldPen = static_cast<HPEN>(SelectObject(dc, basePen));
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
            Ellipse(dc, circle.left, circle.top, circle.right, circle.bottom);
            SelectObject(dc, oldPen);
            DeleteObject(basePen);

            const double fraction = std::clamp(seconds / static_cast<double>(kRestSeconds), 0.0, 1.0);
            const double angle = 6.283185307179586 * fraction - 1.5707963267948966;
            const int radius = 86;
            const POINT start{centerX, centerY - radius};
            const POINT end{centerX + static_cast<int>(radius * std::cos(angle)),
                            centerY + static_cast<int>(radius * std::sin(angle))};
            HPEN accentPen = CreatePen(PS_SOLID, 8, RGB(102, 201, 168));
            oldPen = static_cast<HPEN>(SelectObject(dc, accentPen));
            Arc(dc, circle.left, circle.top, circle.right, circle.bottom,
                start.x, start.y, end.x, end.y);
            SelectObject(dc, oldPen);
            DeleteObject(accentPen);
            SelectObject(dc, oldBrush);

            std::wstring number = std::to_wstring(seconds);
            SetTextColor(dc, RGB(235, 247, 242));
            HFONT oldFont = static_cast<HFONT>(SelectObject(dc, g.titleFont));
            RECT numberRect{centerX - 70, centerY - 32, centerX + 70, centerY + 32};
            DrawTextW(dc, number.c_str(), -1, &numberRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            SelectObject(dc, g.uiFont);
            SetTextColor(dc, RGB(194, 218, 209));
            RECT title{0, centerY - 160, client.right, centerY - 115};
            DrawTextW(dc, L"看看远处，让眼睛慢下来", -1, &title, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            RECT hint{0, centerY + 92, client.right, centerY + 120};
            DrawTextW(dc, L"眨几次眼睛，放松肩颈，自然呼吸", -1, &hint, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            FinishRest();
            return 0;
        case WM_DESTROY:
            KillTimer(window, kRestTimer);
            if (g.restWindow == window) g.restWindow = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK SettingsWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HWND label = CreateWindowExW(0, L"STATIC", L"连续使用电脑多久后提醒：",
                                         WS_CHILD | WS_VISIBLE, 28, 76, 235, 24,
                                         window, nullptr, g.instance, nullptr);
            SetControlFont(label);

            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(g.settings.workMinutes).c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                                        274, 70, 70, 31, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(CtlWorkMinutes)),
                                        g.instance, nullptr);
            SetControlFont(edit);
            SendMessageW(edit, EM_SETLIMITTEXT, 3, 0);

            HWND suffix = CreateWindowExW(0, L"STATIC", L"分钟（1–180）",
                                          WS_CHILD | WS_VISIBLE, 355, 76, 100, 24,
                                          window, nullptr, g.instance, nullptr);
            SetControlFont(suffix);

            HWND strict = CreateWindowExW(0, L"BUTTON", L"严格模式：到时直接进入 20 秒休息界面",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          28, 126, 380, 28, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(CtlStrict)),
                                          g.instance, nullptr);
            SetControlFont(strict);
            SendMessageW(strict, BM_SETCHECK, g.settings.strict ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND startup = CreateWindowExW(0, L"BUTTON", L"登录 Windows 后自动启动",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                           28, 166, 300, 28, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(CtlAutoStart)),
                                           g.instance, nullptr);
            SetControlFont(startup);
            SendMessageW(startup, BM_SETCHECK, g.settings.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);

            CreateButton(window, CmdSaveSettings, L"保存", 250, 236, 88, 36);
            CreateButton(window, CmdCancelSettings, L"取消", 350, 236, 88, 36);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == CmdCancelSettings) {
                DestroyWindow(window);
                return 0;
            }
            if (LOWORD(wParam) == CmdSaveSettings) {
                std::array<wchar_t, 16> text{};
                GetWindowTextW(GetDlgItem(window, CtlWorkMinutes), text.data(), static_cast<int>(text.size()));
                wchar_t* end = nullptr;
                const long value = wcstol(text.data(), &end, 10);
                if (end == text.data() || *end != L'\0' || value < 1 || value > 180) {
                    MessageBoxW(window, L"提醒间隔请输入 1 到 180 之间的整数。", kAppName, MB_OK | MB_ICONINFORMATION);
                    SetFocus(GetDlgItem(window, CtlWorkMinutes));
                    return 0;
                }

                Settings next = g.settings;
                next.workMinutes = static_cast<int>(value);
                next.strict = SendMessageW(GetDlgItem(window, CtlStrict), BM_GETCHECK, 0, 0) == BST_CHECKED;
                next.autoStart = SendMessageW(GetDlgItem(window, CtlAutoStart), BM_GETCHECK, 0, 0) == BST_CHECKED;

                if (next.autoStart != g.settings.autoStart && !SetAutoStart(next.autoStart)) {
                    MessageBoxW(window, L"无法更新开机启动设置，请检查当前账户权限。", kAppName, MB_OK | MB_ICONWARNING);
                    return 0;
                }
                g.settings = next;
                SaveSettings();
                UpdateTrayTooltip();
                DestroyWindow(window);
                return 0;
            }
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, RGB(248, 250, 249));
            SetTextColor(dc, RGB(45, 59, 55));
            static HBRUSH brush = CreateSolidBrush(RGB(248, 250, 249));
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillSolid(reinterpret_cast<HDC>(wParam), client, RGB(248, 250, 249));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(30, 49, 44));
            HFONT old = static_cast<HFONT>(SelectObject(dc, g.titleFont));
            RECT title{28, 20, 430, 55};
            DrawTextW(dc, L"保持专注，也记得眺望", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, old);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (g.settingsWindow == window) g.settingsWindow = nullptr;
            g.lastTick = GetTickCount64();
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterClasses() {
    WNDCLASSEXW base{};
    base.cbSize = sizeof(base);
    base.hInstance = g.instance;
    base.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    base.hIcon = g.trayIcon;

    base.lpfnWndProc = MainWndProc;
    base.lpszClassName = kMainClass;
    if (!RegisterClassExW(&base)) return false;

    base.lpfnWndProc = BannerWndProc;
    base.lpszClassName = kBannerClass;
    base.hbrBackground = nullptr;
    if (!RegisterClassExW(&base)) return false;

    base.lpfnWndProc = RestWndProc;
    base.lpszClassName = kRestClass;
    if (!RegisterClassExW(&base)) return false;

    base.lpfnWndProc = SettingsWndProc;
    base.lpszClassName = kSettingsClass;
    base.hbrBackground = nullptr;
    if (!RegisterClassExW(&base)) return false;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\EyeRest.SingleInstance.9F03A6C1");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g.instance = instance;
    g.trayIcon = LoadIconW(nullptr, IDI_INFORMATION);
    g.uiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.titleFont = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    LoadSettings();
    if (g.settings.autoStart) SetAutoStart(true); // Refresh the path if the executable was moved.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!RegisterClasses()) {
        MessageBoxW(nullptr, L"应用初始化失败。", kAppName, MB_OK | MB_ICONERROR);
        DeleteObject(g.uiFont);
        DeleteObject(g.titleFont);
        CloseHandle(mutex);
        return 1;
    }

    // A hidden top-level window receives TaskbarCreated after Explorer restarts;
    // a message-only window would miss that broadcast.
    g.mainWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kAppName, WS_POPUP,
                                   0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g.mainWindow) {
        DeleteObject(g.uiFont);
        DeleteObject(g.titleFont);
        CloseHandle(mutex);
        return 1;
    }

    g.lastTick = GetTickCount64();
    AddTrayIcon();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!g.settingsWindow || !IsDialogMessageW(g.settingsWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    DeleteObject(g.uiFont);
    DeleteObject(g.titleFont);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
