#include "save_dialog.h"
#include "log.h"
#include "cursor_release.h"

#include <Windows.h>
#include <commdlg.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <thread>

namespace {
std::atomic_bool g_busy{false};
std::mutex g_mutex;
std::string g_status;
std::uint64_t g_statusVersion = 0;
std::wstring g_lastDirectory;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    if (length > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr,
                            nullptr);
    return out;
}

void SetStatus(std::string text) {
    Log::Writef("Save dialog: %s", text.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = std::move(text);
    ++g_statusVersion;
}

bool WriteWholeFile(const std::wstring& path, const std::vector<std::uint8_t>& bytes, DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + done, chunk, &written, nullptr) || written != chunk) {
            error = GetLastError();
            ok = false;
            break;
        }
        done += chunk;
    }
    CloseHandle(file);
    return ok;
}

// The dialog's owner is a hidden topmost window of the dialog's own thread, centred on the game window. Owning it by
// the game window instead joined the two threads' input state: the game's hidden cursor (ShowCursor / SetCursor on
// its thread) was the dialog's too. The owner keeps the dialog in front of the game, and CursorRelease::HoldCursor
// stops the game from hiding and confining the cursor while the dialog is open.
HWND CreateDialogOwner(const RECT& gameRect) {
    const int x = (gameRect.left + gameRect.right) / 2;
    const int y = (gameRect.top + gameRect.bottom) / 2;
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, x, y, 0, 0, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

void RunDialog(std::vector<std::uint8_t> bytes, std::wstring fileName, std::wstring filterName, std::wstring extension,
               RECT gameRect) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    CursorRelease::HoldCursor(true);
    ClipCursor(nullptr);
    HWND owner = CreateDialogOwner(gameRect);

    // Filter: "<name> (*.<ext>)\0*.<ext>\0All files\0*.*\0\0"
    std::wstring filter = filterName + L" (*." + extension + L")";
    filter.push_back(L'\0');
    filter += L"*." + extension;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    wchar_t path[MAX_PATH * 4]{};
    wcsncpy_s(path, fileName.c_str(), _TRUNCATE);
    std::wstring initialDirectory;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        initialDirectory = g_lastDirectory;
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter.c_str();
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
    dialog.lpstrDefExt = extension.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ENABLESIZING;

    if (GetSaveFileNameW(&dialog)) {
        const std::wstring chosen = path;
        DWORD error = 0;
        if (WriteWholeFile(chosen, bytes, error)) {
            const std::size_t slash = chosen.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_lastDirectory = chosen.substr(0, slash);
            }
            SetStatus("Saved to " + Narrow(chosen));
        } else {
            char text[64]{};
            std::snprintf(text, sizeof(text), " (error %lu)", static_cast<unsigned long>(error));
            SetStatus("Could not write " + Narrow(chosen) + text);
        }
    } else {
        const DWORD error = CommDlgExtendedError();
        if (error) {
            char text[96]{};
            std::snprintf(text, sizeof(text), "The save dialog failed (error 0x%lX)", static_cast<unsigned long>(error));
            SetStatus(text);
        } else {
            SetStatus("Save cancelled");
        }
    }

    if (owner) DestroyWindow(owner);
    CursorRelease::HoldCursor(false);
    if (SUCCEEDED(com)) CoUninitialize();
    g_busy.store(false, std::memory_order_release);
}
}  // namespace

namespace SaveDialog {
bool Start(std::vector<std::uint8_t> bytes, const std::string& defaultFileName, const wchar_t* filterName,
           const wchar_t* extension) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    // Where to put the dialog: over the game window (the foreground window of this process).
    RECT gameRect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    HWND game = GetForegroundWindow();
    DWORD gameProcess = 0;
    if (game) GetWindowThreadProcessId(game, &gameProcess);
    if (game && gameProcess == GetCurrentProcessId()) GetWindowRect(game, &gameRect);
    try {
        std::thread(RunDialog, std::move(bytes), Widen(defaultFileName), std::wstring(filterName ? filterName : L"File"),
                    std::wstring(extension ? extension : L"bin"), gameRect)
            .detach();
    } catch (...) {
        g_busy.store(false, std::memory_order_release);
        SetStatus("The save dialog could not be opened");
        return false;
    }
    return true;
}

bool Busy() { return g_busy.load(std::memory_order_acquire); }

std::string Status(std::uint64_t* version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (version) *version = g_statusVersion;
    return g_status;
}
}  // namespace SaveDialog
