#include "speech.h"
#include "spectrum.h"

#include <mmsystem.h>
#include <nemo_speech/asr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace talktext {
namespace {

constexpr int kSampleRate = SpectrumAnalyzer::sampleRate;
constexpr size_t kCaptureSamples = kSampleRate / 40;
// 25 ms level updates, with the same 800 ms driver safety margin as before.
constexpr size_t kCaptureBuffers = 32;
constexpr size_t kDecodeSamples = kSampleRate * 56 / 100;
constexpr size_t kQueueSamples = kSampleRate * 30;
constexpr size_t kDisplayLimit = 4096;

enum class Request { Run, Stop, Cancel };
struct Cancelled {};

std::wstring fromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Speech engine returned invalid UTF-8 text.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string toUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode file path or device message.");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

[[noreturn]] void winError(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

void checkAudio(MMRESULT result, const char* operation) {
    if (result == MMSYSERR_NOERROR) return;
    wchar_t message[MAXERRORLENGTH]{};
    waveInGetErrorTextW(result, message, MAXERRORLENGTH);
    throw std::runtime_error(std::string(operation) + ": " + toUtf8(message) +
                             " (" + std::to_string(result) +
                             "). Check the default microphone and Windows microphone privacy settings.");
}

void checkAsr(nemo_speech_asr_status status, const char* operation) {
    if (status == NEMO_SPEECH_ASR_OK) return;
    // The error is thread-local and expires at the next native call.
    const char* message = nemo_speech_asr_last_error();
    throw std::runtime_error(std::string(operation) + ": " + (message ? message : "Native speech error"));
}

SpeechEvent errorEvent() {
    try { throw; }
    catch (const std::exception& error) {
        try { return {SpeechEvent::Kind::Error, fromUtf8(error.what())}; }
        catch (...) { return {SpeechEvent::Kind::Error, L"Speech processing failed with an invalid error message."}; }
    }
    catch (...) { return {SpeechEvent::Kind::Error, L"Unexpected failure in the local speech engine."}; }
}

class Event {
public:
    explicit Event(bool manualReset) : handle_(CreateEventW(nullptr, manualReset, FALSE, nullptr)) {
        if (!handle_) winError("Create audio event");
    }
    ~Event() { CloseHandle(handle_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

class AudioPipe {
public:
    AudioPipe() : samples_(kQueueSamples) {}

    void push(std::span<const int16_t> input) {
        if (input.empty()) return;
        std::lock_guard lock(mutex_);
        if (input.size() > samples_.size() - count_) {
            throw std::runtime_error("Audio processing fell more than 30 seconds behind. "
                                     "Recording stopped instead of losing speech. Close GPU-heavy apps and try again.");
        }
        size_t write = (read_ + count_) % samples_.size();
        for (const auto sample : input) {
            samples_[write] = static_cast<float>(sample) / 32768.0f;
            write = (write + 1) % samples_.size();
        }
        count_ += input.size();
        available_.notify_one();
    }

    size_t read(std::span<float> output, bool& finished) {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [this, &output] { return count_ >= output.size() || closed_ || error_; });
        if (error_) std::rethrow_exception(error_);
        const size_t size = std::min(output.size(), count_);
        const size_t first = std::min(size, samples_.size() - read_);
        std::copy_n(samples_.data() + read_, first, output.data());
        std::copy_n(samples_.data(), size - first, output.data() + first);
        read_ = (read_ + size) % samples_.size();
        count_ -= size;
        finished = closed_ && count_ == 0;
        return size;
    }

    void fail(std::exception_ptr error) {
        std::lock_guard lock(mutex_);
        if (!error_) error_ = std::move(error);
        available_.notify_one();
    }

    void check() {
        std::lock_guard lock(mutex_);
        if (error_) std::rethrow_exception(error_);
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        available_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable available_;
    std::vector<float> samples_;
    size_t read_ = 0;
    size_t count_ = 0;
    bool closed_ = false;
    std::exception_ptr error_;
};

class Capture {
public:
    explicit Capture(std::function<void(SpeechEvent)> post)
        : post_(std::move(post)), requestEvent_(true), audioEvent_(false), haltEvent_(true) {}
    ~Capture() { halt(); }

    void start() { thread_ = std::thread([this] { run(); }); }

    void stop(bool cancel) {
        if (cancel) request.store(Request::Cancel);
        else {
            auto expected = Request::Run;
            request.compare_exchange_strong(expected, Request::Stop);
        }
        if (!SetEvent(requestEvent_.get())) winError("Signal microphone stop");
    }

    void halt() noexcept {
        SetEvent(haltEvent_.get());
        if (thread_.joinable()) thread_.join();
    }

    void checkCancelled() const {
        if (request.load() == Request::Cancel) throw Cancelled{};
    }

    AudioPipe pipe;
    std::atomic<Request> request{Request::Run};

private:
    struct Buffer {
        WAVEHDR header{};
        std::array<int16_t, kCaptureSamples> samples{};
        bool prepared = false;
    };

    void level(std::span<const int16_t> samples) {
        if (samples.empty()) return;
        double energy = 0.0;
        for (const auto sample : samples) {
            const float value = static_cast<float>(sample) / 32768.0f;
            energy += value * value;
        }
        const float rms = static_cast<float>(std::sqrt(energy / samples.size()));
        spectrum_.push(samples);
        post_({SpeechEvent::Kind::Level, {}, std::min(1.0f, rms * 4.0f), spectrum_.bands()});
    }

    void consume(bool requeue) {
        if (requeue && std::all_of(buffers_.begin(), buffers_.end(), [](const Buffer& buffer) {
                return (buffer.header.dwFlags & WHDR_DONE) != 0;
            })) {
            throw std::runtime_error("Microphone capture buffers were exhausted. "
                                     "Recording stopped instead of silently dropping audio.");
        }
        for (size_t i = 0; i < buffers_.size(); ++i) {
            auto& buffer = buffers_[next_];
            if (!(buffer.header.dwFlags & WHDR_DONE)) break;
            if (buffer.header.dwBytesRecorded > sizeof(buffer.samples) || buffer.header.dwBytesRecorded % 2) {
                throw std::runtime_error("The microphone driver returned an invalid audio buffer.");
            }
            const std::span<const int16_t> samples(buffer.samples.data(), buffer.header.dwBytesRecorded / sizeof(int16_t));
            pipe.push(samples);
            level(samples);
            if (requeue) {
                buffer.header.dwBytesRecorded = 0;
                checkAudio(waveInAddBuffer(device_, &buffer.header, sizeof(WAVEHDR)), "Queue microphone buffer");
            }
            next_ = (next_ + 1) % buffers_.size();
        }
    }

    void record() {
        if (request.load() != Request::Run) return;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = sizeof(int16_t);
        format.nAvgBytesPerSec = kSampleRate * sizeof(int16_t);
        checkAudio(waveInOpen(&device_, WAVE_MAPPER, &format,
                              reinterpret_cast<DWORD_PTR>(audioEvent_.get()), 0, CALLBACK_EVENT), "Open microphone");
        for (auto& buffer : buffers_) {
            buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
            buffer.header.dwBufferLength = static_cast<DWORD>(sizeof(buffer.samples));
            checkAudio(waveInPrepareHeader(device_, &buffer.header, sizeof(WAVEHDR)), "Prepare microphone buffer");
            buffer.prepared = true;
            checkAudio(waveInAddBuffer(device_, &buffer.header, sizeof(WAVEHDR)), "Queue microphone buffer");
        }
        if (request.load() != Request::Run) return;
        checkAudio(waveInStart(device_), "Start microphone");
        post_({SpeechEvent::Kind::Listening, {}});

        const HANDLE events[]{requestEvent_.get(), haltEvent_.get(), audioEvent_.get()};
        while (request.load() == Request::Run) {
            const DWORD wait = WaitForMultipleObjects(3, events, FALSE, 5000);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) break;
            if (wait == WAIT_FAILED) winError("Wait for microphone");
            if (wait == WAIT_TIMEOUT) {
                throw std::runtime_error("The microphone stopped providing audio. Check its connection and try again.");
            }
            consume(true);
        }
        checkAudio(waveInReset(device_), "Stop microphone");
        reset_ = true;
        // Reset returns even the partly filled final buffer, in recording order.
        if (request.load() != Request::Cancel && WaitForSingleObject(haltEvent_.get(), 0) != WAIT_OBJECT_0) {
            consume(false);
        }
    }

    void cleanup() {
        if (!device_) return;
        auto collect = [this](MMRESULT result, const char* operation) {
            try { checkAudio(result, operation); }
            catch (...) { pipe.fail(std::current_exception()); }
        };
        if (!reset_) collect(waveInReset(device_), "Reset microphone");
        for (auto& buffer : buffers_) {
            if (buffer.prepared) {
                collect(waveInUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR)), "Release microphone buffer");
            }
        }
        collect(waveInClose(device_), "Close microphone");
        device_ = nullptr;
    }

    void run() noexcept {
        try { record(); }
        catch (...) { pipe.fail(std::current_exception()); }
        cleanup();
        pipe.close();
    }

    std::function<void(SpeechEvent)> post_;
    Event requestEvent_;
    Event audioEvent_;
    Event haltEvent_;
    std::thread thread_;
    HWAVEIN device_ = nullptr;
    std::array<Buffer, kCaptureBuffers> buffers_{};
    SpectrumAnalyzer spectrum_;
    size_t next_ = 0;
    bool reset_ = false;
};

std::wstring displayTail(std::wstring_view text) {
    if (text.size() <= kDisplayLimit) return std::wstring(text);
    size_t start = text.size() - (kDisplayLimit - 2);
    if (text[start] >= 0xdc00 && text[start] <= 0xdfff) ++start;
    return L"\u2026 " + std::wstring(text.substr(start));
}

std::wstring interimText(std::string_view text) {
    // Only the display suffix needs UTF-16 conversion; finals retain all text.
    constexpr size_t maxBytes = kDisplayLimit * 4;
    if (text.size() > maxBytes) {
        size_t start = text.size() - maxBytes;
        while ((static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
        text.remove_prefix(start);
    }
    return displayTail(fromUtf8(text));
}

class Stream {
public:
    explicit Stream(nemo_speech_asr_recognizer* recognizer) {
        auto options = nemo_speech_asr_recognition_options_default();
        options.language_code = "en-US";
        options.interim_results = true;
        options.enable_automatic_punctuation = true;
        nemo_speech_asr_stream* stream = nullptr;
        checkAsr(nemo_speech_asr_streaming_recognize(recognizer, &options, &stream), "Open speech stream");
        stream_.reset(stream);
    }

    void accept(std::span<const float> samples, int rate) {
        checkAsr(nemo_speech_asr_stream_push_f32(stream_.get(), samples.data(), samples.size(), rate), "Buffer speech audio");
        drain();
    }

    std::wstring partial() const {
        if (finalized_.empty()) return current_;
        std::wstring text = displayTail(finalized_);
        if (!current_.empty()) {
            text.push_back(L' ');
            text += current_;
        }
        return displayTail(text);
    }

    std::wstring finish() {
        checkAsr(nemo_speech_asr_stream_finish(stream_.get()), "Finish speech stream");
        drain();
        return std::move(finalized_);
    }

private:
    void drain() {
        for (;;) {
            nemo_speech_asr_result* raw = nullptr;
            checkAsr(nemo_speech_asr_stream_next(stream_.get(), &raw), "Decode speech audio");
            std::unique_ptr<nemo_speech_asr_result, decltype(&nemo_speech_asr_result_destroy)>
                result(raw, &nemo_speech_asr_result_destroy);
            if (!result) return;
            if (!nemo_speech_asr_result_alternative_count(result.get())) {
                throw std::runtime_error("Speech engine returned a result without a transcript.");
            }
            const char* text = nemo_speech_asr_result_transcript(result.get(), 0);
            if (!text) throw std::runtime_error("Speech engine returned an invalid transcript.");
            if (nemo_speech_asr_result_is_final(result.get())) {
                const auto segment = fromUtf8(text);
                if (!segment.empty()) {
                    if (!finalized_.empty()) finalized_.push_back(L' ');
                    finalized_ += segment;
                }
                current_.clear();
            } else {
                // Interim replaces the current utterance; it is not a new segment.
                current_ = interimText(text);
            }
        }
    }

    std::unique_ptr<nemo_speech_asr_stream, decltype(&nemo_speech_asr_stream_close)>
        stream_{nullptr, &nemo_speech_asr_stream_close};
    std::wstring finalized_;
    std::wstring current_;
};

class Engine {
public:
    explicit Engine(const std::filesystem::path& root) {
        const auto path = root / L"models/nemotron/nemotron-speech-streaming-en-0.6b.q8_0.gguf";
        if (!std::filesystem::is_regular_file(path) || !std::filesystem::file_size(path)) {
            throw std::runtime_error("Missing speech model: " + toUtf8(path.wstring()) +
                                     ". Run python tools/setup.py, or restore the models folder beside TalkText.exe.");
        }
        const auto utf8 = toUtf8(path.wstring());
        nemo_speech_asr_backend_config backend{};
        backend.size = sizeof(backend);
        backend.gpu = 0;  // CUDA is required; the runtime rejects unavailable GPU backends.
        nemo_speech_asr_model_config model{};
        model.size = sizeof(model);
        model.path = utf8.c_str();
        nemo_speech_asr_streaming_config streaming{};
        streaming.size = sizeof(streaming);
        streaming.chunk_size = 0.56f;
        streaming.rnnt_right_context = 6;
        nemo_speech_asr_endpointing_config endpointing{};
        endpointing.size = sizeof(endpointing);
        endpointing.enable = false;  // One growing utterance, no artificial duration reset.
        nemo_speech_asr_batching_config batching{};
        batching.size = sizeof(batching);
        batching.enable = false;
        batching.state_arena_slots = 1;
        nemo_speech_asr_recognizer_config config{};
        config.size = sizeof(config);
        config.backend = &backend;
        config.model = &model;
        config.streaming = &streaming;
        config.endpointing = &endpointing;
        config.batching = &batching;
        nemo_speech_asr_recognizer* recognizer = nullptr;
        checkAsr(nemo_speech_asr_create(&config, &recognizer), "Load CUDA speech model");
        recognizer_.reset(recognizer);
    }

    nemo_speech_asr_recognizer* get() const { return recognizer_.get(); }

    void warmup(const std::atomic<bool>& stopping) {
        Stream stream(get());
        const std::array<float, kSampleRate / 10> silence{};
        for (int i = 0; i < 40; ++i) {
            if (stopping.load()) throw Cancelled{};
            stream.accept(silence, kSampleRate);
        }
        if (stopping.load()) throw Cancelled{};
        stream.finish();
    }

private:
    std::unique_ptr<nemo_speech_asr_recognizer, decltype(&nemo_speech_asr_destroy)>
        recognizer_{nullptr, &nemo_speech_asr_destroy};
};

class WaveReader {
public:
    explicit WaveReader(const std::filesystem::path& path) : file_(path, std::ios::binary) {
        if (!file_) throw std::runtime_error("Cannot open WAV file: " + toUtf8(path.wstring()));
        file_.seekg(0, std::ios::end);
        const auto fileSize = file_.tellg();
        file_.seekg(0);
        std::array<unsigned char, 12> header{};
        readBytes(header.data(), header.size());
        if (std::memcmp(header.data(), "RIFF", 4) || std::memcmp(header.data() + 8, "WAVE", 4)) {
            throw std::runtime_error("Provide a RIFF mono 16-bit PCM WAV file.");
        }
        const uint64_t end = static_cast<uint64_t>(u32(header.data() + 4)) + 8;
        if (fileSize < 0 || end > static_cast<uint64_t>(fileSize) || end < 12) {
            throw std::runtime_error("Truncated WAV container.");
        }
        bool haveFormat = false;
        bool haveData = false;
        uint64_t dataOffset = 0;
        for (uint64_t offset = 12; offset < end;) {
            if (end - offset < 8) throw std::runtime_error("Invalid WAV chunk header.");
            file_.seekg(static_cast<std::streamoff>(offset));
            std::array<unsigned char, 8> chunk{};
            readBytes(chunk.data(), chunk.size());
            const uint32_t size = u32(chunk.data() + 4);
            const uint64_t next = offset + 8 + size + (size & 1);
            if (next > end) throw std::runtime_error("Truncated WAV chunk.");
            if (!std::memcmp(chunk.data(), "fmt ", 4)) {
                if (haveFormat || size < 16) throw std::runtime_error("Invalid WAV format chunk.");
                std::array<unsigned char, 16> format{};
                readBytes(format.data(), format.size());
                rate_ = u32(format.data() + 4);
                if (u16(format.data()) != WAVE_FORMAT_PCM || u16(format.data() + 2) != 1 ||
                    u16(format.data() + 14) != 16 || u16(format.data() + 12) != 2 ||
                    rate_ < 8000 || rate_ > 96000 || u32(format.data() + 8) != rate_ * 2) {
                    throw std::runtime_error("WAV must contain mono 16-bit PCM at 8-96 kHz.");
                }
                haveFormat = true;
            } else if (!std::memcmp(chunk.data(), "data", 4)) {
                if (haveData || !size || size % 2) throw std::runtime_error("Invalid WAV audio chunk.");
                dataOffset = offset + 8;
                remaining_ = size;
                haveData = true;
            }
            offset = next;
        }
        if (!haveFormat || !haveData) throw std::runtime_error("WAV is missing its format or audio chunk.");
        file_.seekg(static_cast<std::streamoff>(dataOffset));
    }

    int rate() const { return static_cast<int>(rate_); }

    size_t read(std::span<float> output) {
        std::array<unsigned char, kDecodeSamples * 2> pcm;
        const size_t count = std::min({output.size(), pcm.size() / 2, static_cast<size_t>(remaining_ / 2)});
        readBytes(pcm.data(), count * 2);
        for (size_t i = 0; i < count; ++i) {
            const int value = u16(pcm.data() + i * 2);
            output[i] = static_cast<float>(value >= 32768 ? value - 65536 : value) / 32768.0f;
        }
        remaining_ -= static_cast<uint32_t>(count * 2);
        return count;
    }

private:
    static uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    static uint32_t u32(const unsigned char* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    void readBytes(unsigned char* output, size_t count) {
        if (!file_.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(count))) {
            throw std::runtime_error("Cannot read WAV audio: truncated or unreadable file.");
        }
    }
    std::ifstream file_;
    uint32_t rate_ = 0;
    uint32_t remaining_ = 0;
};

}  // namespace

struct SpeechSession::Impl {
    mutable std::mutex lifecycle;
    std::condition_variable work;
    std::atomic<bool> healthy{false};
    std::atomic<bool> suppressEvents{false};
    std::atomic<bool> stopping{false};
    std::thread worker;
    HWND receiver = nullptr;
    std::filesystem::path root;
    bool preparing = false;
    bool loadRequested = false;
    std::shared_ptr<Capture> current;

    void post(SpeechEvent event) const noexcept {
        if (suppressEvents.load()) return;
        try {
            auto payload = std::make_unique<SpeechEvent>(std::move(event));
            if (!PostMessageW(receiver, WM_SPEECH, 0, reinterpret_cast<LPARAM>(payload.get()))) {
                OutputDebugStringW(L"TalkText: receiver unavailable for speech event.\n");
                return;
            }
            payload.release();
        } catch (...) {
            OutputDebugStringW(L"TalkText: could not allocate speech event.\n");
        }
    }

    void complete(const std::shared_ptr<Capture>& capture, SpeechEvent terminal) {
        // No new session can begin until the old microphone and native stream close.
        capture->halt();
        std::lock_guard lock(lifecycle);
        if (capture->request.load() == Request::Cancel && terminal.kind == SpeechEvent::Kind::Complete) {
            terminal = {SpeechEvent::Kind::Cancelled, {}};
        }
        current.reset();
        post(std::move(terminal));
    }

    void transcribe(Engine& engine, const std::shared_ptr<Capture>& capture) {
        SpeechEvent terminal{SpeechEvent::Kind::Complete, {}};
        try {
            capture->checkCancelled();
            capture->pipe.check();
            Stream stream(engine.get());
            std::array<float, kDecodeSamples> samples{};
            std::wstring previous;
            bool finished = false;
            bool receivedAudio = false;
            while (!finished) {
                capture->checkCancelled();
                const auto size = capture->pipe.read(samples, finished);
                capture->checkCancelled();
                if (size) {
                    receivedAudio = true;
                    stream.accept({samples.data(), size}, kSampleRate);
                    capture->checkCancelled();
                    auto text = stream.partial();
                    if (text != previous) {
                        previous = text;
                        post({SpeechEvent::Kind::Partial, std::move(text)});
                    }
                }
            }
            if (receivedAudio) terminal.text = stream.finish();
            capture->checkCancelled();
        } catch (const Cancelled&) {
            terminal = {SpeechEvent::Kind::Cancelled, {}};
        } catch (...) {
            terminal = errorEvent();
        }
        complete(capture, std::move(terminal));
    }

    void run() noexcept {
        // Only this thread touches the recognizer, its streams, or its CUDA context.
        std::unique_ptr<Engine> engine;
        for (;;) {
            std::shared_ptr<Capture> capture;
            std::filesystem::path loadRoot;
            bool load = false;
            {
                std::unique_lock lock(lifecycle);
                work.wait(lock, [this] { return stopping.load() || loadRequested || current; });
                if (stopping.load()) break;
                load = std::exchange(loadRequested, false);
                if (load) loadRoot = root;
                else capture = current;
            }
            if (load) {
                try {
                    post({SpeechEvent::Kind::Status, L"Loading CUDA speech model..."});
                    engine = std::make_unique<Engine>(loadRoot);
                    if (stopping.load()) throw Cancelled{};
                    post({SpeechEvent::Kind::Status, L"Warming CUDA speech model..."});
                    engine->warmup(stopping);
                    std::lock_guard lock(lifecycle);
                    preparing = false;
                    if (!stopping.load()) {
                        healthy.store(true);
                        post({SpeechEvent::Kind::Ready, {}});
                    }
                } catch (...) {
                    auto terminal = errorEvent();
                    engine.reset();
                    {
                        std::lock_guard lock(lifecycle);
                        healthy.store(false);
                        preparing = false;
                        capture = current;
                        if (!capture) post(terminal);
                    }
                    if (capture) complete(capture, std::move(terminal));
                }
            } else if (capture) {
                transcribe(*engine, capture);
            }
        }
        std::shared_ptr<Capture> capture;
        {
            std::lock_guard lock(lifecycle);
            capture = std::move(current);
            healthy.store(false);
        }
        if (capture) capture->halt();
    }
};

SpeechSession::SpeechSession() : impl_(std::make_unique<Impl>()) {}
SpeechSession::~SpeechSession() { shutdown(); }

void SpeechSession::prepare(HWND receiver, const std::filesystem::path& appDirectory) {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->stopping.load()) throw std::logic_error("Speech engine is shutting down.");
    if (!IsWindow(receiver)) throw std::invalid_argument("Speech event receiver is not a valid window.");
    if (impl_->worker.joinable() && (receiver != impl_->receiver || appDirectory != impl_->root)) {
        throw std::logic_error("Shut down the speech engine before changing its receiver or model directory.");
    }
    if (impl_->healthy.load() || impl_->preparing) return;
    if (impl_->current) throw std::logic_error("A dictation session is still finishing.");
    impl_->receiver = receiver;
    impl_->root = appDirectory;
    impl_->suppressEvents.store(false);
    impl_->preparing = true;
    impl_->loadRequested = true;
    try {
        if (!impl_->worker.joinable()) impl_->worker = std::thread([self = impl_.get()] { self->run(); });
    } catch (...) {
        impl_->preparing = false;
        impl_->loadRequested = false;
        throw;
    }
    impl_->work.notify_one();
}

void SpeechSession::start() {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->stopping.load() || impl_->current) throw std::logic_error("A dictation session is already active or shutting down.");
    if (!impl_->healthy.load() && !impl_->preparing) throw std::logic_error("Prepare the speech engine before recording.");
    auto capture = std::make_shared<Capture>([self = impl_.get()](SpeechEvent event) { self->post(std::move(event)); });
    // Starts the microphone independently, even while the worker is loading/warming.
    capture->start();
    impl_->current = std::move(capture);
    impl_->work.notify_one();
}

void SpeechSession::stop(bool cancel) {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->current) impl_->current->stop(cancel);
}

bool SpeechSession::active() const {
    std::lock_guard lock(impl_->lifecycle);
    return static_cast<bool>(impl_->current);
}

bool SpeechSession::ready() const { return impl_->healthy.load(); }

void SpeechSession::shutdown() {
    std::thread worker;
    HWND receiver;
    {
        std::lock_guard lock(impl_->lifecycle);
        if (impl_->stopping.exchange(true)) return;
        impl_->suppressEvents.store(true);
        impl_->healthy.store(false);
        if (impl_->current) impl_->current->stop(true);
        impl_->work.notify_one();
        worker = std::move(impl_->worker);
        receiver = impl_->receiver;
    }
    if (worker.joinable()) worker.join();
    if (receiver && GetWindowThreadProcessId(receiver, nullptr) == GetCurrentThreadId()) {
        MSG message{};
        while (PeekMessageW(&message, receiver, WM_SPEECH, WM_SPEECH, PM_REMOVE)) {
            delete reinterpret_cast<SpeechEvent*>(message.lParam);
        }
    }
    std::lock_guard lock(impl_->lifecycle);
    impl_->receiver = nullptr;
    impl_->root.clear();
    impl_->preparing = false;
    impl_->loadRequested = false;
    impl_->stopping.store(false);
}

std::wstring SpeechSession::transcribeFile(const std::filesystem::path& appDirectory,
                                         const std::filesystem::path& wav) {
    WaveReader reader(wav);
    Engine engine(appDirectory);
    Stream stream(engine.get());
    std::array<float, kDecodeSamples> samples{};
    while (const auto count = reader.read(samples)) {
        stream.accept({samples.data(), count}, reader.rate());
    }
    return stream.finish();
}

}  // namespace talktext
