#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace talktext {

inline constexpr UINT WM_SPEECH = WM_APP + 1;

struct SpeechEvent {
    enum class Kind { Status, Ready, Listening, Level, Partial, Complete, Cancelled, Error };
    Kind kind;
    // Partial: latest 4096 UTF-16 units. Complete: full native punctuated/cased transcript.
    // Level: no text; min(1, 4 * PCM RMS) and eight Hann FFT bands, independent of inference.
    // Spectrum: 80/160/320/640/1000/1600/2500/4000/8000 Hz edges; [0,1] band RMS * 4.
    std::wstring text;
    float level = 0.0f;
    std::array<float, 8> spectrum{};
};

class SpeechSession {
public:
    SpeechSession();
    ~SpeechSession();
    SpeechSession(const SpeechSession&) = delete;
    SpeechSession& operator=(const SpeechSession&) = delete;

    // Receiver owns each SpeechEvent* posted in WM_SPEECH's lParam.
    // Load and warm once on a persistent CUDA worker; explicit retry after failure.
    void prepare(HWND receiver, const std::filesystem::path& appDirectory);
    bool ready() const;
    // Capture starts asynchronously, including while prepare is still running.
    void start();
    void stop(bool cancel);
    bool active() const;

    // Call on the receiver's UI thread before destroying its window.
    // Cancels, joins, unloads, and deletes queued events. Allows later prepare().
    void shutdown();

    // Real CUDA streaming ASR; mono 16-bit PCM RIFF WAV at 8-96 kHz. Throws on failure.
    static std::wstring transcribeFile(const std::filesystem::path& appDirectory,
                                      const std::filesystem::path& wav);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace talktext
