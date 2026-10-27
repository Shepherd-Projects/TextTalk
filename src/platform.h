#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace talktext {

inline constexpr UINT WM_HOTKEY_EVENT = WM_APP + 2;
inline constexpr UINT WM_HOTKEY_CAPTURE = WM_APP + 3;

enum class InsertMethod { Paste, Type, ClipboardOnly };

struct Settings {
    bool startWithWindows = false;
    bool keepInTray = true;
    bool startHidden = false;
    bool pushToTalk = false;
    bool autoEnter = false;
    InsertMethod method = InsertMethod::Paste;
    size_t historyLimit = 100;
    std::vector<UINT> hotkey{VK_CONTROL, VK_LWIN, VK_SPACE};
    // Orchard appearance: preset index (theme::Preset) and frosted glass percent (0 = off).
    uint32_t theme = 2;
    uint32_t frostedGlass = 0;
};

struct HistoryEntry {
    std::wstring timestamp;
    std::wstring text;
};

Settings loadSettings();
void saveSettings(const Settings& settings);
std::vector<HistoryEntry> loadHistory();
void saveHistory(const std::vector<HistoryEntry>& history);
void setStartup(bool enabled);
std::filesystem::path appDirectory();
std::wstring keyName(UINT key);
std::wstring chordName(const std::vector<UINT>& keys);

// Call on the OLE-initialized UI thread, after releasing the hotkey.
// ClipboardOnly deliberately replaces the clipboard and never sends Enter.
std::wstring insertText(HWND target, const std::wstring& text, InsertMethod method,
                        bool autoEnter);

// Recovery is UI-thread-only. Keep the app open while a snapshot is pending.
bool hasPendingClipboard();
std::wstring restorePendingClipboard();

// Install, update, and destroy on the receiver's message-loop thread.
// Events: 1 = chord down, 2 = chord up, 3 = Escape. Only capture consumes keys.
class Hotkeys {
public:
    Hotkeys();
    ~Hotkeys();
    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    void install(HWND receiver, const std::vector<UINT>& keys);
    void uninstall();
    void setChord(const std::vector<UINT>& keys);
    void capture(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace talktext
