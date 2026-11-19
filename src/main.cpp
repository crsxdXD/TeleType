#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <ole2.h>
#include <atomic>
#include <algorithm>
#include <memory>
#include <cwctype>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"
#include "hotkeys.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_DONE = WM_APP + 2;
constexpr UINT WM_ACCEPT = WM_APP + 3;
constexpr UINT TIMER_SUGGEST = 1;
constexpr UINT TIMER_FIX = 2;
constexpr UINT TIMER_HIDE = 3;
constexpr UINT TIMER_ACCEPT = 4;
constexpr UINT CMD_PAUSE = 101;
constexpr UINT CMD_EXIT = 102;
constexpr UINT CMD_HELP = 103;

HWND mainWindow = nullptr;
HWND popup = nullptr;
HHOOK keyboardHook = nullptr;
HHOOK mouseHook = nullptr;
HFONT popupFont = nullptr;
NOTIFYICONDATAW tray{};
std::wstring typed;
std::wstring suggestion;
std::wstring sourceForFix;
std::wstring modelName;
std::atomic_bool requestBusy{false};
unsigned long long version = 0;
ULONGLONG lastRequestAt = 0;
bool paused = false;
bool fixingPreview = false;
bool popupError = false;
bool settingText = false;

struct ApiResult {
    unsigned long long requestedVersion;
    bool fix;
    std::wstring text;
    std::wstring error;
};

std::wstring env(const wchar_t* name) {
    DWORD sz = GetEnvironmentVariableW(name, nullptr, 0);
    if (!sz) return {};
    std::wstring data(sz, L'\0');
    DWORD actual = GetEnvironmentVariableW(name, data.data(), sz);
    if (!actual || actual >= sz) return {};
    data.resize(actual);
    return data;
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    if (!n) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
    if (!n) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), out.data(), n);
    return out;
}
bool isTelegramForeground() {
    HWND front = GetForegroundWindow();
    if (!front || front == popup || front == mainWindow) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t filename[2048]; DWORD length = _countof(filename);
    BOOL ok = QueryFullProcessImageNameW(process, 0, filename, &length);
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(filename, L'\\');
    name = name ? name + 1 : filename;
    return _wcsicmp(name, L"Telegram.exe") == 0;
}
void hidePopup() {
    if (mainWindow) { KillTimer(mainWindow, TIMER_HIDE); KillTimer(mainWindow, TIMER_ACCEPT); }
    suggestion.clear();
    sourceForFix.clear();
    fixingPreview = false;
    popupError = false;
    if (popup) ShowWindow(popup, SW_HIDE);
}
void clearTracked() {
    typed.clear();
    ++version;
    KillTimer(mainWindow, TIMER_SUGGEST);
    hidePopup();
}
void updateTray() {
    wcscpy_s(tray.szTip, paused ? L"TeleType - paused" : L"TeleType - running");
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}
void repositionPopup() {
    HWND front = GetForegroundWindow();
    RECT area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    RECT win{}; GetWindowRect(front, &win);
    int x = win.left + 24;
    int y = win.bottom - 135;
    GUITHREADINFO info{sizeof(info)};
    DWORD threadId = GetWindowThreadProcessId(front, nullptr);
    if (GetGUIThreadInfo(threadId, &info) && info.hwndCaret &&
        (info.rcCaret.left != info.rcCaret.right || info.rcCaret.top != info.rcCaret.bottom)) {
        POINT point{info.rcCaret.left, info.rcCaret.bottom};
        ClientToScreen(info.hwndCaret, &point);
        if (point.x >= win.left && point.x <= win.right && point.y >= win.top && point.y <= win.bottom) {
            x = point.x; y = point.y + 11;
        }
    }
    int width = 540, height = 80;
    const int left = static_cast<int>(area.left) + 4;
    const int top = static_cast<int>(area.top) + 4;
    x = std::clamp(x, left, std::max(left, static_cast<int>(area.right) - width - 4));
    y = std::clamp(y, top, std::max(top, static_cast<int>(area.bottom) - height - 4));
    SetWindowPos(popup, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void showPopup(const std::wstring& value, bool isFix, bool isError = false) {
    if (!isTelegramForeground() || paused || value.empty()) return;
    suggestion = value;
    fixingPreview = isFix;
    popupError = isError;
    InvalidateRect(popup, nullptr, TRUE);
    repositionPopup();
    if (isFix) SetTimer(mainWindow, TIMER_HIDE, 15000, nullptr);
}

void keyEvent(WORD vk, bool release) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = release ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(input));
}
void shortcut(WORD vk, bool shift = false) {
    keyEvent(VK_CONTROL, false);
    if (shift) keyEvent(VK_SHIFT, false);
    keyEvent(vk, false); keyEvent(vk, true);
    if (shift) keyEvent(VK_SHIFT, true);
    keyEvent(VK_CONTROL, true);
}
void typeText(const std::wstring& value) {
    settingText = true;
    for (wchar_t ch : value) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            // Telegram uses Shift+Enter to insert a line break without sending the message.
            keyEvent(VK_SHIFT, false);
            keyEvent(VK_RETURN, false); keyEvent(VK_RETURN, true);
            keyEvent(VK_SHIFT, true);
            continue;
        }
        INPUT keys[2]{};
        keys[0].type = INPUT_KEYBOARD;
        keys[0].ki.wVk = 0;
        keys[0].ki.wScan = ch;
        keys[0].ki.dwFlags = KEYEVENTF_UNICODE;
        keys[1] = keys[0];
        keys[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, keys, sizeof(INPUT));
    }
    settingText = false;
}
std::wstring readSelectedDraft() {
    // Preserve all clipboard formats through the Windows OLE clipboard interface.
    IDataObject* original = nullptr;
    const HRESULT saved = OleGetClipboard(&original);
    // If we cannot preserve the clipboard, never overwrite it.
    if (FAILED(saved) || !original) return {};
    const DWORD before = GetClipboardSequenceNumber();
    shortcut('A');
    shortcut('C');
    Sleep(150);  // Telegram processes SendInput asynchronously.
    std::wstring content;
    // Reject stale clipboard data if Telegram did not copy the selected draft.
    const bool changed = (GetClipboardSequenceNumber() != before);
    if (changed) {
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (OpenClipboard(mainWindow)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* chars = static_cast<const wchar_t*>(GlobalLock(h));
                    if (chars) { content = chars; GlobalUnlock(h); }
                }
                CloseClipboard();
                break;
            }
            Sleep(15);
        }
    }
    // Restore every clipboard format, including images and application data.
    OleSetClipboard(original);
    original->Release();
    // Clear select-all without touching the draft.
    keyEvent(VK_RIGHT, false); keyEvent(VK_RIGHT, true);
    return content;
}

// The LLM server is hard-coded to the loopback address. Drafts never go to a
// remote HTTP service, and this application needs no cloud account or API key.
std::wstring localCompletion(const std::wstring& input, bool fix, std::wstring& error) {
    if (input.empty()) return {};
    HINTERNET session = WinHttpOpen(L"TeleType/0.2 (local only)",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"Cannot initialize local HTTP"; return {}; }
    // A fast connection check, but allow a small CPU model time to answer.
    WinHttpSetTimeouts(session, 2500, 1500, 10000, fix ? 45000 : 25000);
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", 11434, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/api/chat",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    // Disable HTTP redirects; a local service must not redirect drafts to the internet.
    bool safeToSend = false;
    if (request) {
        DWORD flags = WINHTTP_DISABLE_REDIRECTS;
        safeToSend = WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                                     &flags, sizeof(flags)) != FALSE;
    }
    std::wstring instruction = fix
        ? L"You proofread an unsent chat message. Correct spelling, grammar and punctuation ONLY. "
          L"Keep the same language (Russian, Ukrainian or English), meaning, style, slang and emoji. "
          L"Never translate, expand or censor. Output only the FULL corrected text."
        : L"You are inline autocomplete for an unsent Telegram chat message. Write ONLY the "
          L"next natural 2-10 words, continuing the text exactly where it ends. Output only a "
          L"SUFFIX, never repeat the original text. Keep the same language (Russian, Ukrainian "
          L"or English), tone, slang and emoji. Use a leading space if needed. No quotes, "
          L"markdown, explanation, or newline.";
    const std::string body =
        "{\"model\":\"" + tinyjson::escape(utf8(modelName)) +
        "\",\"stream\":false,\"think\":false,\"keep_alive\":\"10m\",\"messages\":[" +
        "{\"role\":\"system\",\"content\":\"" + tinyjson::escape(utf8(instruction)) + "\"}," +
        "{\"role\":\"user\",\"content\":\"" + tinyjson::escape(utf8(input)) + "\"}]" +
        ",\"options\":{\"temperature\":" + (fix ? "0.1" : "0.45") +
        ",\"num_ctx\":1024,\"num_predict\":" + (fix ? "512" : "64") + "}}";
    bool ok = false;
    DWORD status = 0;
    std::string response;
    if (request && safeToSend) {
        if (WinHttpSendRequest(request, L"Content-Type: application/json\r\n", (DWORD)-1,
                               const_cast<char*>(body.data()), (DWORD)body.size(),
                               (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD len = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            char buf[4096]; DWORD bytes = 0;
            bool readOK = true;
            for (;;) {
                if (!WinHttpReadData(request, buf, sizeof(buf), &bytes)) { readOK = false; break; }
                if (bytes == 0) break;
                if (response.size() + bytes > 256 * 1024) { readOK = false; break; }
                response.append(buf, bytes);
            }
            ok = readOK && (status >= 200 && status < 300);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok) {
        if (!safeToSend) error = L"Cannot safely configure local HTTP request";
        else if (status == 404) error = L"Ollama model missing. Run: ollama pull " + modelName;
        else if (status) error = L"Local Ollama HTTP " + std::to_wstring(status);
        else error = L"Ollama unavailable. Install/start Ollama (port 11434).";
        return {};
    }
    std::string answer;
    if (!tinyjson::ollama_chat_text(response, answer)) {
        error = L"Unexpected response from local Ollama"; return {};
    }
    std::wstring result = wide(answer);
    if (!fix && !result.empty()) {
        // Prevent the model from retyping the entire draft in the suggestion.
        if (result.compare(0, input.size(), input) == 0) result.erase(0, input.size());
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    }
    return result;
}

void startRequest(bool fix, const std::wstring& source) {
    if (requestBusy.exchange(true)) return;
    lastRequestAt = GetTickCount64();
    const unsigned long long capturedVersion = version;
    std::thread([fix, source, capturedVersion] {
        auto* result = new ApiResult{capturedVersion, fix, {}, {}};
        result->text = localCompletion(source, fix, result->error);
        if (!PostMessageW(mainWindow, WM_DONE, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}
void triggerFix() {
    if (paused || !isTelegramForeground() || requestBusy.load()) return;
    hidePopup();
    std::wstring full = readSelectedDraft();
    // Keep local inference responsive and avoid accidentally processing huge drafts.
    if (full.empty() || full.size() > 1800) {
        showPopup(full.empty() ? L"No draft found. Click the chat input first." : L"Draft too long (max 1800 characters).", false, true);
        SetTimer(mainWindow, TIMER_HIDE, 5000, nullptr);
        return;
    }
    typed = full.substr(full.size() > 600 ? full.size() - 600 : 0);
    ++version;
    sourceForFix = full;
    startRequest(true, full);
}
void acceptSuggestion() {
    if (suggestion.empty() || popupError || paused || !isTelegramForeground()) return;
    const std::wstring answer = suggestion;
    const bool fix = fixingPreview;
    hidePopup();
    if (fix) {
        // User explicitly confirms replacement. Never send Telegram's Enter alone.
        shortcut('A');
        typeText(answer);
        typed = answer.substr(answer.size() > 600 ? answer.size() - 600 : 0);
    } else {
        typeText(answer);
        typed += answer;
        if (typed.size() > 600) typed.erase(0, typed.size() - 600);
    }
    ++version;
    KillTimer(mainWindow, TIMER_SUGGEST);
}

LRESULT CALLBACK hook(int code, WPARAM message, LPARAM payload) {
    if (code < 0) return CallNextHookEx(keyboardHook, code, message, payload);
    auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(payload);
    if ((key->flags & LLKHF_INJECTED) || settingText) return CallNextHookEx(keyboardHook, code, message, payload);
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return CallNextHookEx(keyboardHook, code, message, payload);
    if (paused || !isTelegramForeground()) {
        if (!typed.empty() || !suggestion.empty()) clearTracked();
        return CallNextHookEx(keyboardHook, code, message, payload);
    }
    const DWORD vk = key->vkCode;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    const bool rightAlt = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0; // AltGr
    const auto action = teletype_hotkeys::match(vk, ctrl, alt, shift, win,
                                                rightAlt, !suggestion.empty(), popupError);
    if (action == teletype_hotkeys::Action::FixDraft) {
        SetTimer(mainWindow, TIMER_FIX, 100, nullptr);
        return 1;
    }
    if (action == teletype_hotkeys::Action::AcceptSuggestion) {
        // Delay text insertion until Ctrl/Alt are released; never insert while
        // the Telegram composer still sees those modifier keys as pressed.
        SetTimer(mainWindow, TIMER_ACCEPT, 100, nullptr);
        return 1;
    }
    // Modifiers on their own are not commands. Do not invalidate the visible
    // suggestion while the user is holding Ctrl+Alt to accept it.
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL) {
        return CallNextHookEx(keyboardHook, code, message, payload);
    }
    // Tab and Shift+Tab always belong to Telegram's focus navigation.
    if (vk == VK_TAB || vk == VK_ESCAPE) {
        clearTracked();
        return CallNextHookEx(keyboardHook, code, message, payload);
    }
    if ((ctrl || alt || win) && !rightAlt) {
        // Ctrl+Tab, Alt+Up, Ctrl+1 etc. can switch chats. Clear old completion.
        clearTracked();
        return CallNextHookEx(keyboardHook, code, message, payload);
    }
    if (vk == VK_RETURN) { clearTracked(); return CallNextHookEx(keyboardHook, code, message, payload); }
    if (vk == VK_BACK) {
        if (!typed.empty()) typed.pop_back();
    } else if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END || vk == VK_DELETE) {
        clearTracked(); return CallNextHookEx(keyboardHook, code, message, payload);
    } else {
        BYTE state[256]{};
        GetKeyboardState(state);
        state[vk] |= 0x80;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] |= 0x80;
        else state[VK_SHIFT] &= ~0x80;
        HWND front = GetForegroundWindow();
        HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(front, nullptr));
        wchar_t buf[8]{};
        int count = ToUnicodeEx(vk, key->scanCode, state, buf, _countof(buf), 0, layout);
        if (count > 0) typed.append(buf, std::min(count, (int)_countof(buf)));
        else return CallNextHookEx(keyboardHook, code, message, payload);
    }
    ++version;
    hidePopup();
    if (typed.size() > 600) typed.erase(0, typed.size() - 600);
    KillTimer(mainWindow, TIMER_SUGGEST);
    if (typed.size() >= 8) SetTimer(mainWindow, TIMER_SUGGEST, 900, nullptr);
    return CallNextHookEx(keyboardHook, code, message, payload);
}

// Moving the caret or opening another Telegram chat invalidates a pending
// suggestion. This is deliberately conservative: it never captures mouse data.
LRESULT CALLBACK mouseHookProc(int code, WPARAM message, LPARAM payload) {
    if (code >= 0 && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                      message == WM_MBUTTONDOWN)) {
        if (!typed.empty() || !suggestion.empty() || !sourceForFix.empty()) clearTracked();
    }
    return CallNextHookEx(mouseHook, code, message, payload);
}

LRESULT CALLBACK overlayProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT rc{}; GetClientRect(window, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(31, 37, 50));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT previous = static_cast<HFONT>(SelectObject(dc, popupFont));
        RECT title{14, 8, rc.right - 14, 30};
        SetTextColor(dc, RGB(128, 201, 248));
        std::wstring titleText = popupError ? L"TELETYPE ERROR" : (fixingPreview ? L"CORRECTION  •  Ctrl+Alt+J to replace" : L"SUGGESTION  •  Ctrl+Alt+J to insert");
        DrawTextW(dc, titleText.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT body{14, 34, rc.right - 14, rc.bottom - 10};
        SetTextColor(dc, RGB(239, 243, 250));
        DrawTextW(dc, suggestion.c_str(), -1, &body, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        SelectObject(dc, previous);
        EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
LRESULT CALLBACK mainProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_CREATE: return 0;
        case WM_TIMER:
            if (w == TIMER_FIX) {
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
                KillTimer(window, TIMER_FIX);
                triggerFix();
            } else if (w == TIMER_ACCEPT) {
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
                KillTimer(window, TIMER_ACCEPT);
                acceptSuggestion();
            } else if (w == TIMER_SUGGEST) {
                KillTimer(window, TIMER_SUGGEST);
                if (paused || !isTelegramForeground() || typed.size() < 8 || requestBusy.load()) return 0;
                ULONGLONG elapsed = GetTickCount64() - lastRequestAt;
                if (elapsed < 4000) {
                    SetTimer(window, TIMER_SUGGEST, (UINT)(4000 - elapsed), nullptr);
                    return 0;
                }
                startRequest(false, typed);
            } else if (w == TIMER_HIDE) {
                KillTimer(window, TIMER_HIDE);
                if (fixingPreview || popupError) hidePopup();
            }
            return 0;
        case WM_DONE: {
            std::unique_ptr<ApiResult> result(reinterpret_cast<ApiResult*>(l));
            requestBusy = false;
            if (result->requestedVersion != version || paused || !isTelegramForeground()) return 0;
            if (!result->error.empty()) {
                showPopup(result->error, false, true);
                SetTimer(window, TIMER_HIDE, 5000, nullptr);
            } else if (result->fix) {
                if (result->text != sourceForFix) showPopup(result->text, true);
                else hidePopup();
            } else if (!result->text.empty() && result->text.size() <= 140 &&
                       result->text.find_first_of(L"\r\n") == std::wstring::npos) {
                showPopup(result->text, false);
            }
            return 0;
        }
        case WM_ACCEPT: acceptSuggestion(); return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case CMD_PAUSE:
                    paused = !paused; clearTracked(); updateTray(); return 0;
                case CMD_HELP:
                    MessageBoxW(nullptr,
                        L"TeleType: type in Telegram Desktop, pause about 1 second.\n"
                        L"Ctrl+Alt+J: accept a suggested completion or correction.\n"
                        L"Ctrl+Alt+G: correct your entire unsent draft.\n"
                        L"Tab and Shift+Tab keep their normal Telegram behavior.\n"
                        L"Right-click this tray icon to pause or quit.\n\n"
                        L"All inference stays local in Ollama (127.0.0.1). No cloud API or Telegram account access.",
                        L"TeleType help", MB_OK | MB_ICONINFORMATION); return 0;
                case CMD_EXIT: DestroyWindow(window); return 0;
            }
            return 0;
        case WM_TRAY:
            if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
                POINT p{}; GetCursorPos(&p);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, CMD_HELP, L"Help / Hotkeys");
                AppendMenuW(menu, MF_STRING, CMD_PAUSE, paused ? L"Resume" : L"Pause");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");
                SetForegroundWindow(window);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, window, nullptr);
                DestroyMenu(menu);
            }
            return 0;
        case WM_DESTROY:
            if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
            if (mouseHook) UnhookWindowsHookEx(mouseHook);
            Shell_NotifyIconW(NIM_DELETE, &tray);
            if (popup) DestroyWindow(popup);
            if (popupFont) DeleteObject(popupFont);
            PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\TeleTypeSingleInstanceV1");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) CloseHandle(singleton);
        return 0;
    }
    OleInitialize(nullptr);
    modelName = env(L"TELETYPE_MODEL");
    if (modelName.empty()) modelName = L"qwen3:0.6b";
    WNDCLASSW wc{};
    wc.hInstance = instance; wc.lpfnWndProc = mainProc;
    wc.lpszClassName = L"TeleTypeHost";
    RegisterClassW(&wc);
    mainWindow = CreateWindowW(wc.lpszClassName, L"TeleType", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, instance, nullptr);
    WNDCLASSW pc{};
    pc.hInstance = instance; pc.lpfnWndProc = overlayProc;
    pc.lpszClassName = L"TeleTypePopup";
    RegisterClassW(&pc);
    popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
        WS_EX_LAYERED | WS_EX_TRANSPARENT, pc.lpszClassName, L"", WS_POPUP, 0, 0, 540, 80,
        nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(popup, 0, 244, LWA_ALPHA);
    popupFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    tray.cbSize = sizeof(tray);
    tray.hWnd = mainWindow;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray.szTip, L"TeleType - running");
    Shell_NotifyIconW(NIM_ADD, &tray);
    keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, hook, instance, 0);
    mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, instance, 0);
    if (!keyboardHook || !mouseHook) {
        MessageBoxW(nullptr, L"Could not install the input listener.", L"TeleType", MB_OK | MB_ICONERROR);
        DestroyWindow(mainWindow);
        OleUninitialize();
        CloseHandle(singleton);
        return 1;
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    CloseHandle(singleton);
    return 0;
}
