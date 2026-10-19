// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG
// SPDX-License-Identifier: GPL-2.0-or-later

// BBSC mod: the Windows side of bbsc_platform.h, and the DLL's entry point. bbsc-launch.exe (or a
// mod manager) loads bbsc.dll into the emulator; DllMain starts the worker that finds the game.

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "bbsc_platform.h"
#include "common/logging/log.h"

namespace BbscMod {
void RunWorker(); // bbsc_mod.cpp
}

namespace BbscMod::Platform {
namespace {

HMODULE self_module = nullptr;

u32 BitsFromProtect(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0) {
        return 0;
    }
    switch (protect & 0xFF) {
    case PAGE_READONLY:
        return ProtRead;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        return ProtRead | ProtWrite;
    case PAGE_EXECUTE_READ:
        return ProtRead | ProtExec;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return ProtRead | ProtWrite | ProtExec;
    case PAGE_EXECUTE:
        return ProtExec;
    default:
        return 0;
    }
}

DWORD ProtectFromBits(u32 prot) {
    const bool read = (prot & ProtRead) != 0;
    const bool write = (prot & ProtWrite) != 0;
    if ((prot & ProtExec) != 0) {
        return write ? PAGE_EXECUTE_READWRITE : read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    return write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
}

std::wstring Wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(size > 0 ? size : 0, L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
        out.resize(size - 1);
    }
    return out;
}

} // namespace

bool QueryRegion(u64 address, Region& out) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    out.start = reinterpret_cast<u64>(info.BaseAddress);
    out.end = out.start + info.RegionSize;
    out.mapped = info.State == MEM_COMMIT;
    out.prot = out.mapped ? BitsFromProtect(info.Protect) : 0;
    return true;
}

std::vector<u64> ExecutableRegions() {
    constexpr DWORD Executable =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    std::vector<u64> starts;
    MEMORY_BASIC_INFORMATION info{};
    u64 at = 0x10000;
    while (at < 0x7FFF'FFFF'0000ULL &&
           VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != 0) {
        const u64 base = reinterpret_cast<u64>(info.BaseAddress);
        if (info.State == MEM_COMMIT && (info.Protect & Executable) != 0 &&
            (info.Protect & PAGE_GUARD) == 0) {
            starts.push_back(base);
        }
        at = base + info.RegionSize;
    }
    return starts;
}

bool Protect(u64 address, u64 size, u32 prot, u32* restore) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, ProtectFromBits(prot), &old)) {
        return false;
    }
    if (restore != nullptr) {
        *restore = old;
    }
    return true;
}

void Restore(u64 address, u64 size, u32 restore) {
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, restore, &ignored);
}

void FlushCode(u64 address, u64 size) {
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
}

u64 MapExecutable(u64 address, u64 size) {
    return reinterpret_cast<u64>(VirtualAlloc(reinterpret_cast<void*>(address), size,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
}

std::filesystem::path ModulePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self_module, path, MAX_PATH);
    return path;
}

std::filesystem::path ExecutablePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

u32 ProcessId() {
    return GetCurrentProcessId();
}

void SetEnvironment(const char* name, const char* value) {
    _putenv_s(name, value);
    SetEnvironmentVariableA(name, value);
}

void ShowError(const std::string& title, const std::string& text) {
    MessageBoxW(nullptr, Wide(text).c_str(), Wide(title).c_str(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

std::filesystem::path InstalledUserFolder() {
    if (const wchar_t* appdata = _wgetenv(L"APPDATA")) {
        return std::filesystem::path{appdata} / L"shadPS4";
    }
    return {};
}

u32 HttpPost(const std::string& url, const std::string& body, const std::string& headers) {
    const std::wstring wide = Wide(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        return 0;
    }
    DWORD status = 0;
    HINTERNET session = WinHttpOpen(L"BBSC", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request =
        connect
            ? WinHttpOpenRequest(connect, L"POST", path, nullptr, nullptr, nullptr,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : nullptr;
    if (request != nullptr) {
        WinHttpSetTimeouts(request, 1000, 1000, 1000, 1500);
        const std::wstring header_lines = Wide(headers);
        if (WinHttpSendRequest(request, header_lines.c_str(), static_cast<DWORD>(-1),
                               const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &status, &size, nullptr);
        }
    }
    for (HINTERNET h : {request, connect, session}) {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
    return status;
}

// The Discord app's named pipe, \\.\pipe\discord-ipc-N.
s64 DiscordIpcOpen(int index) {
    const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
    const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    return pipe == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<s64>(pipe);
}

bool DiscordIpcWrite(s64 handle, const void* data, size_t size) {
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle), data, static_cast<DWORD>(size), &written,
                     nullptr) &&
           written == size;
}

bool DiscordIpcRead(s64 handle, void* data, size_t size) {
    DWORD got = 0;
    return size == 0 || (ReadFile(reinterpret_cast<HANDLE>(handle), data, static_cast<DWORD>(size),
                                  &got, nullptr) &&
                         got == size);
}

bool DiscordIpcAvailable(s64 handle, size_t& bytes) {
    DWORD available = 0;
    if (!PeekNamedPipe(reinterpret_cast<HANDLE>(handle), nullptr, 0, nullptr, &available,
                       nullptr)) {
        return false;
    }
    bytes = available;
    return true;
}

void DiscordIpcClose(s64 handle) {
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

// ---- Keyboard and mouse
// -------------------------------------------------------------------------- A mod sees no window
// events, so the keys are polled, only while this process's window is in front (Nucleus runs two).
// While the camera has the mouse, CineWindowProc keeps the keys and the mouse from the game
// (2026-10-12; before, they reached it too). 2026-10-05 (the user: better mouse controls): the
// mouse looks around without a button held - the cursor is hidden and kept in the middle of the
// game window - and the wheel sets the speed (the game window is subclassed for WM_MOUSEWHEEL /
// WM_SETCURSOR while the camera is on).

namespace {

int VirtualKey(Key key) {
    switch (key) {
    case Key::F1:
        return VK_F1;
    case Key::F8:
        return VK_F8;
    case Key::W:
        return 'W';
    case Key::A:
        return 'A';
    case Key::S:
        return 'S';
    case Key::D:
        return 'D';
    case Key::E:
        return 'E';
    case Key::Q:
        return 'Q';
    case Key::C:
        return 'C';
    case Key::Z:
        return 'Z';
    case Key::X:
        return 'X';
    case Key::V:
        return 'V';
    case Key::Space:
        return VK_SPACE;
    case Key::Shift:
        return VK_SHIFT;
    case Key::Alt:
        return VK_MENU;
    case Key::Comma:
        return VK_OEM_COMMA;
    case Key::Period:
        return VK_OEM_PERIOD;
    case Key::Slash:
        return VK_OEM_2;
    case Key::Up:
        return VK_UP;
    case Key::Down:
        return VK_DOWN;
    case Key::Left:
        return VK_LEFT;
    case Key::Right:
        return VK_RIGHT;
    case Key::PageUp:
        return VK_PRIOR;
    case Key::PageDown:
        return VK_NEXT;
    case Key::LeftBracket:
        return VK_OEM_4;
    case Key::RightBracket:
        return VK_OEM_6;
    case Key::Count:
        break;
    }
    return 0;
}

bool cine_looking = false;
POINT cine_anchor{};
HWND cine_window = nullptr;                // the subclassed game window (SDL's "SDL_app")
WNDPROC cine_window_proc = nullptr;        // its own window procedure
std::atomic<bool> cine_mouse_owned{false}; // the camera is on and has the mouse
std::atomic<int> cine_wheel{0};            // wheel notches since the last TakeMouseLook

bool InFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

LRESULT CALLBACK CineWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (cine_mouse_owned) {
        if (message == WM_MOUSEWHEEL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            cine_wheel += delta > 0 ? 1 : (delta < 0 ? -1 : 0);
            return 0; // not for the game
        }
        if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        // 2026-10-12 (the user: "each button does a million things"): the camera's keys walked,
        // dodged and used items at the same time, and the mouse turned the game's camera. While
        // the camera is on, presses and movement stop here; the emulator (SDL) never sees them.
        // Releases still go through, so a key held when the camera came on is not left stuck.
        // The camera reads the keys itself (GetAsyncKeyState), so it is not affected. Alt+F4
        // passes.
        switch (message) {
        case WM_KEYDOWN:
        case WM_CHAR:
        case WM_DEADCHAR:
        case WM_SYSCHAR:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEMOVE:
            return 0;
        case WM_SYSKEYDOWN:
            if (wparam != VK_F4) {
                return 0;
            }
            break;
        case WM_INPUT: {
            // Raw input (SDL's relative mouse): drop movement and presses, keep releases.
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                bool release = false;
                if (raw.header.dwType == RIM_TYPEKEYBOARD) {
                    release = (raw.data.keyboard.Flags & RI_KEY_BREAK) != 0;
                } else if (raw.header.dwType == RIM_TYPEMOUSE) {
                    constexpr USHORT Ups = RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_UP |
                                           RI_MOUSE_BUTTON_3_UP | RI_MOUSE_BUTTON_4_UP |
                                           RI_MOUSE_BUTTON_5_UP;
                    release = (raw.data.mouse.usButtonFlags & Ups) != 0;
                }
                if (!release) {
                    return DefWindowProcW(window, message, wparam, lparam);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return CallWindowProcW(cine_window_proc, window, message, wparam, lparam);
}

// The game window when it is in front, else nullptr. Subclassed the first time it is seen.
HWND CineGameWindowInFront() {
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (window == nullptr || pid != GetCurrentProcessId()) {
        return nullptr;
    }
    char cls[32]{};
    GetClassNameA(window, cls, sizeof(cls));
    if (std::strcmp(cls, "SDL_app") != 0) {
        return nullptr; // e.g. the console
    }
    if (cine_window != window) {
        if (cine_window != nullptr && cine_window_proc != nullptr) {
            SetWindowLongPtrW(cine_window, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(cine_window_proc));
        }
        cine_window = window;
        cine_window_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CineWindowProc)));
        LOG_INFO(Debug, "BBSC cinematic camera: mouse wheel and cursor hooked on the game window");
    }
    return window;
}

} // namespace

bool KeyDown(Key key) {
    return InFront() && (GetAsyncKeyState(VirtualKey(key)) & 0x8000) != 0;
}

// Mouse look, no button needed: the cursor's movement, put back in the middle of the game window
// each frame (hidden there by CineWindowProc).
MouseLook TakeMouseLook() {
    MouseLook look;
    look.wheel = cine_wheel.exchange(0);
    POINT now{};
    RECT client{};
    HWND window = InFront() ? CineGameWindowInFront() : nullptr;
    if (window != nullptr && GetClientRect(window, &client) && GetCursorPos(&now)) {
        POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
        ClientToScreen(window, &center);
        if (cine_looking) {
            look.dx = static_cast<float>(now.x - cine_anchor.x);
            look.dy = static_cast<float>(now.y - cine_anchor.y);
        }
        cine_anchor = center;
        cine_looking = true;
        SetCursorPos(center.x, center.y);
        cine_mouse_owned = true;
        look.active = true;
    } else {
        cine_looking = false; // another window is in front: the mouse is the user's
        cine_mouse_owned = false;
    }
    return look;
}

void ReleaseMouse() {
    cine_looking = false;
    cine_mouse_owned = false;
    cine_wheel = 0;
}

} // namespace BbscMod::Platform

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        BbscMod::Platform::self_module = module;
        DisableThreadLibraryCalls(module);
        const auto worker = [](void*) -> DWORD {
            BbscMod::RunWorker();
            return 0;
        };
        if (HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
