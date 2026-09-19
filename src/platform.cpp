#include "platform.h"

#include <ole2.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace talktext {
namespace {

using Microsoft::WRL::ComPtr;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring widen(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) return L"Unknown platform error.";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    result.pop_back();
    return result;
}

std::wstring systemMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result;
    if (length) {
        result.assign(buffer, length);
        LocalFree(buffer);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                                  result.back() == L' ')) result.pop_back();
    } else {
        wchar_t number[24]{};
        swprintf_s(number, L"0x%08lX", code);
        result = number;
    }
    return result;
}

[[noreturn]] void fail(const std::wstring& message) {
    throw std::runtime_error(utf8(message));
}

[[noreturn]] void winFail(const std::wstring& action, DWORD error = GetLastError()) {
    fail(action + L": " + systemMessage(error));
}

void requireHr(HRESULT result, const std::wstring& action) {
    if (FAILED(result)) winFail(action, static_cast<DWORD>(result));
}

std::filesystem::path executablePath() {
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                               static_cast<DWORD>(path.size()));
        if (!length) winFail(L"Cannot locate TalkText executable");
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= 32768) fail(L"TalkText executable path is too long.");
        path.resize(std::min<size_t>(32768, path.size() * 2));
    }
}

std::filesystem::path dataPath(const wchar_t* filename) {
    PWSTR folder = nullptr;
    requireHr(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                  &folder), L"Cannot locate LocalAppData");
    std::filesystem::path path;
    try {
        path = std::filesystem::path(folder) / L"TalkText";
    } catch (...) {
        CoTaskMemFree(folder);
        throw;
    }
    CoTaskMemFree(folder);
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) fail(L"Cannot create TalkText data folder: " + widen(error.message().c_str()));
    return path / filename;
}

class FileReader {
public:
    explicit FileReader(const std::filesystem::path& path)
        : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (!file_) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
            winFail(L"Cannot read " + path.wstring(), error);
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(), &size)) winFail(L"Cannot read saved file size");
        remaining_ = static_cast<uint64_t>(size.QuadPart);
    }
    bool exists() const noexcept { return static_cast<bool>(file_); }
    uint64_t remaining() const noexcept { return remaining_; }
    void read(void* output, size_t bytes) {
        if (bytes > remaining_) fail(L"Saved TalkText data is truncated or has invalid lengths.");
        auto* next = static_cast<unsigned char*>(output);
        while (bytes) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 1024 * 1024));
            DWORD done = 0;
            if (!ReadFile(file_.get(), next, chunk, &done, nullptr)) winFail(L"Cannot read saved TalkText data");
            if (done != chunk) fail(L"Saved TalkText data changed or ended while being read.");
            next += done;
            bytes -= done;
            remaining_ -= done;
        }
    }
    template<class T> T number() {
        T value{};
        read(&value, sizeof(value));
        return value;
    }
    std::wstring string() {
        const uint64_t length = number<uint64_t>();
        std::wstring value;
        if (length > remaining_ / sizeof(wchar_t) || length > value.max_size())
            fail(L"Saved history contains an invalid string length.");
        value.resize(static_cast<size_t>(length));
        read(value.data(), value.size() * sizeof(wchar_t));
        return value;
    }
    void header(const std::array<char, 8>& expected) {
        std::array<char, 8> actual{};
        read(actual.data(), actual.size());
        if (actual != expected) fail(L"Saved TalkText data has an unsupported format or version.");
    }
    void finish() {
        if (remaining_) fail(L"Saved TalkText data contains unexpected trailing bytes.");
    }
private:
    Handle file_;
    uint64_t remaining_ = 0;
};

class AtomicFile {
public:
    explicit AtomicFile(std::filesystem::path destination) : destination_(std::move(destination)) {
        static std::atomic<unsigned long> serial{0};
        temporary_ = destination_;
        temporary_ += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(serial.fetch_add(1));
        file_.reset(CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!file_) winFail(L"Cannot create temporary settings/history file");
    }
    ~AtomicFile() {
        file_.reset();
        if (!temporary_.empty()) DeleteFileW(temporary_.c_str());
    }
    void write(const void* input, size_t bytes) {
        const auto* next = static_cast<const unsigned char*>(input);
        while (bytes) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 1024 * 1024));
            DWORD done = 0;
            if (!WriteFile(file_.get(), next, chunk, &done, nullptr)) winFail(L"Cannot save TalkText data");
            if (done != chunk) fail(L"The saved data could not be written completely.");
            next += done;
            bytes -= done;
        }
    }
    template<class T> void number(T value) { write(&value, sizeof(value)); }
    void string(const std::wstring& value) {
        number<uint64_t>(value.size());
        write(value.data(), value.size() * sizeof(wchar_t));
    }
    void commit() {
        if (!FlushFileBuffers(file_.get())) winFail(L"Cannot flush saved TalkText data");
        file_.reset();
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            winFail(L"Cannot replace saved TalkText data; previous file was retained");
        temporary_.clear();
    }
private:
    std::filesystem::path destination_;
    std::filesystem::path temporary_;
    Handle file_;
};

constexpr std::array<char, 8> settingsHeader{'T', 'T', 'S', 'E', 'T', '0', '1', '\0'};
constexpr std::array<char, 8> historyHeader{'T', 'T', 'H', 'I', 'S', '0', '1', '\0'};

UINT normalizeKey(UINT key) noexcept {
    switch (key) {
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    case VK_RWIN: return VK_LWIN;
    default: return key;
    }
}

std::vector<UINT> normalizedChord(const std::vector<UINT>& keys) {
    if (keys.empty()) fail(L"Choose at least one hotkey key or mouse button.");
    std::array<bool, 256> seen{};
    std::vector<UINT> result;
    for (UINT key : keys) {
        key = normalizeKey(key);
        if (key == 0 || key >= 255 || key == VK_ESCAPE || key == VK_PACKET || key == VK_PROCESSKEY)
            fail(L"This key cannot be used in a hotkey. Escape is reserved for cancellation.");
        if (!seen[key]) {
            seen[key] = true;
            result.push_back(key);
        }
    }
    return result;
}

struct Medium {
    STGMEDIUM value{};
    Medium() = default;
    ~Medium() { ReleaseStgMedium(&value); }
    Medium(const Medium&) = delete;
    Medium& operator=(const Medium&) = delete;
    Medium(Medium&& other) noexcept : value(std::exchange(other.value, STGMEDIUM{})) {}
    Medium& operator=(Medium&& other) noexcept {
        if (this != &other) {
            ReleaseStgMedium(&value);
            value = std::exchange(other.value, STGMEDIUM{});
        }
        return *this;
    }
};

HRESULT copyStream(IStream* source, IStream** output) {
    if (!source) return DV_E_STGMEDIUM;
    LARGE_INTEGER zero{};
    ULARGE_INTEGER position{};
    HRESULT result = source->Seek(zero, STREAM_SEEK_CUR, &position);
    if (FAILED(result)) return result;
    STATSTG stat{};
    result = source->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(result)) return result;
    ComPtr<IStream> copy;
    result = CreateStreamOnHGlobal(nullptr, TRUE, &copy);
    if (FAILED(result)) return result;
    result = source->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(result)) return result;
    ULARGE_INTEGER read{}, written{};
    result = source->CopyTo(copy.Get(), stat.cbSize, &read, &written);
    LARGE_INTEGER restore{};
    restore.QuadPart = static_cast<LONGLONG>(position.QuadPart);
    const HRESULT restored = source->Seek(restore, STREAM_SEEK_SET, nullptr);
    if (FAILED(result)) return result;
    if (FAILED(restored)) return restored;
    if (read.QuadPart != stat.cbSize.QuadPart || written.QuadPart != stat.cbSize.QuadPart)
        return STG_E_READFAULT;
    // IDataObject::GetData exposes stream bytes from zero to its returned seek
    // position. Leaving the copy at zero makes OleFlushClipboard publish no data.
    *output = copy.Detach();
    return S_OK;
}

HRESULT cloneMedium(CLIPFORMAT format, const STGMEDIUM& source, STGMEDIUM* output) {
    *output = {};
    Medium copy;
    copy.value.tymed = source.tymed;
    switch (source.tymed) {
    case TYMED_HGLOBAL: {
        if (!source.hGlobal) return DV_E_STGMEDIUM;
        SetLastError(ERROR_SUCCESS);
        const SIZE_T bytes = GlobalSize(source.hGlobal);
        if (!bytes && GetLastError() != ERROR_SUCCESS) return DV_E_STGMEDIUM;
        copy.value.hGlobal = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!copy.value.hGlobal) return E_OUTOFMEMORY;
        if (bytes) {
            const void* from = GlobalLock(source.hGlobal);
            void* to = GlobalLock(copy.value.hGlobal);
            if (from && to) std::memcpy(to, from, bytes);
            if (to) GlobalUnlock(copy.value.hGlobal);
            if (from) GlobalUnlock(source.hGlobal);
            if (!from || !to) return DV_E_STGMEDIUM;
        }
        break;
    }
    case TYMED_GDI:
        if (format != CF_BITMAP && format != CF_DSPBITMAP && format != CF_PALETTE)
            return DV_E_TYMED;
        copy.value.hBitmap = static_cast<HBITMAP>(OleDuplicateData(source.hBitmap,
            format == CF_PALETTE ? CF_PALETTE : CF_BITMAP, 0));
        if (!copy.value.hBitmap) return E_OUTOFMEMORY;
        break;
    case TYMED_MFPICT:
        copy.value.hMetaFilePict = OleDuplicateData(source.hMetaFilePict, CF_METAFILEPICT, 0);
        if (!copy.value.hMetaFilePict) return DV_E_STGMEDIUM;
        break;
    case TYMED_ENHMF:
        copy.value.hEnhMetaFile = CopyEnhMetaFileW(source.hEnhMetaFile, nullptr);
        if (!copy.value.hEnhMetaFile) return DV_E_STGMEDIUM;
        break;
    case TYMED_ISTREAM: {
        const HRESULT result = copyStream(source.pstm, &copy.value.pstm);
        if (FAILED(result)) return result;
        break;
    }
    case TYMED_ISTORAGE: {
        if (!source.pstg) return DV_E_STGMEDIUM;
        ComPtr<ILockBytes> bytes;
        HRESULT result = CreateILockBytesOnHGlobal(nullptr, TRUE, &bytes);
        if (FAILED(result)) return result;
        result = StgCreateDocfileOnILockBytes(bytes.Get(),
            STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &copy.value.pstg);
        if (FAILED(result)) return result;
        result = source.pstg->CopyTo(0, nullptr, nullptr, copy.value.pstg);
        if (FAILED(result)) return result;
        result = copy.value.pstg->Commit(STGC_DEFAULT);
        if (FAILED(result)) return result;
        break;
    }
    default:
        return DV_E_TYMED;
    }
    *output = std::exchange(copy.value, STGMEDIUM{});
    return S_OK;
}

struct ClipboardEntry {
    FORMATETC format{};
    Medium medium;
};

class ClipboardData final : public IDataObject {
public:
    explicit ClipboardData(bool notifyReads = false) {
        if (notifyReads) {
            readEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!readEvent_) winFail(L"Cannot create clipboard delivery event");
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *output = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* requested, STGMEDIUM* output) override {
        if (!requested || !output) return E_POINTER;
        *output = {};
        const ClipboardEntry* entry = find(*requested);
        if (!entry) return DV_E_FORMATETC;
        const HRESULT result = cloneMedium(entry->format.cfFormat, entry->medium.value, output);
        if (SUCCEEDED(result) && notifyReads_ && readEvent_) SetEvent(readEvent_.get());
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DV_E_TYMED; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* requested) override {
        if (!requested) return E_POINTER;
        return find(*requested) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (!output) return E_POINTER;
        output->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        try {
            std::vector<FORMATETC> formats;
            formats.reserve(entries.size());
            for (const auto& entry : entries) formats.push_back(entry.format);
            if (formats.size() > std::numeric_limits<UINT>::max()) return E_OUTOFMEMORY;
            return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), output);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** output) override {
        if (output) *output = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HANDLE readEvent() const noexcept { return readEvent_.get(); }
    void notifyReads(bool enabled) noexcept { notifyReads_ = enabled; }
    std::vector<ClipboardEntry> entries;
private:
    const ClipboardEntry* find(const FORMATETC& requested) const noexcept {
        if (requested.ptd) return nullptr;
        for (const auto& entry : entries) {
            if (entry.format.cfFormat == requested.cfFormat &&
                entry.format.dwAspect == requested.dwAspect && entry.format.lindex == requested.lindex &&
                (entry.format.tymed & requested.tymed)) return &entry;
        }
        return nullptr;
    }
    std::atomic<ULONG> references_{1};
    Handle readEvent_;
    bool notifyReads_ = false;
};

std::wstring clipboardFormatName(UINT format) {
    wchar_t name[256]{};
    if (GetClipboardFormatNameW(format, name, static_cast<int>(std::size(name)))) return name;
    return L"format " + std::to_wstring(format);
}

ComPtr<ClipboardData> captureClipboard(DWORD& sequence) {
    // Rendering delayed data can itself change the sequence. Take one new stable
    // snapshot in that case; never restore a mixture from two clipboard owners.
    for (int attempt = 0; attempt != 2; ++attempt) {
        sequence = GetClipboardSequenceNumber();
        ComPtr<IDataObject> original;
        requireHr(OleGetClipboard(&original), L"Cannot read clipboard; nothing was changed");
        ComPtr<IEnumFORMATETC> formats;
        requireHr(original->EnumFormatEtc(DATADIR_GET, &formats),
                  L"Cannot enumerate every clipboard format; nothing was changed");
        ComPtr<ClipboardData> snapshot;
        snapshot.Attach(new ClipboardData);
        for (;;) {
            FORMATETC format{};
            ULONG count = 0;
            const HRESULT next = formats->Next(1, &format, &count);
            if (next == S_FALSE && count == 0) break;
            requireHr(next, L"Cannot enumerate clipboard formats; nothing was changed");
            if (count != 1) fail(L"Clipboard returned an invalid format list; nothing was changed.");
            const bool deviceSpecific = format.ptd != nullptr;
            CoTaskMemFree(format.ptd);
            format.ptd = nullptr;
            if (deviceSpecific)
                fail(L"Clipboard contains device-specific data that cannot be preserved. Use Unicode typing instead.");
            if (!format.cfFormat) fail(L"Clipboard contains an invalid format; nothing was changed.");
            constexpr DWORD supported = TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE |
                                        TYMED_GDI | TYMED_MFPICT | TYMED_ENHMF;
            format.tymed &= supported;
            if (!format.tymed)
                fail(L"Clipboard contains file-only or unsupported data that cannot be preserved. Use Unicode typing instead.");
            Medium provided;
            requireHr(original->GetData(&format, &provided.value),
                L"Cannot preserve clipboard " + clipboardFormatName(format.cfFormat) + L"; nothing was changed");
            if (!(format.tymed & provided.value.tymed) ||
                (provided.value.tymed & (provided.value.tymed - 1)) != 0)
                fail(L"Clipboard returned an invalid storage medium; nothing was changed.");
            ClipboardEntry entry;
            entry.format = format;
            entry.format.tymed = provided.value.tymed;
            requireHr(cloneMedium(format.cfFormat, provided.value, &entry.medium.value),
                L"Cannot snapshot clipboard " + clipboardFormatName(format.cfFormat) + L"; use Unicode typing instead");
            snapshot->entries.push_back(std::move(entry));
        }
        if (sequence == GetClipboardSequenceNumber()) return snapshot;
    }
    fail(L"Clipboard changed while being copied. Nothing was inserted; try again.");
}

ComPtr<ClipboardData> textClipboard(const std::wstring& text) {
    ComPtr<ClipboardData> object;
    object.Attach(new ClipboardData(true));
    ClipboardEntry entry;
    entry.format = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    entry.medium.value.tymed = TYMED_HGLOBAL;
    if (text.size() >= std::numeric_limits<size_t>::max() / sizeof(wchar_t))
        fail(L"Text is too large for the Windows clipboard.");
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    entry.medium.value.hGlobal = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!entry.medium.value.hGlobal) winFail(L"Cannot allocate clipboard text");
    void* buffer = GlobalLock(entry.medium.value.hGlobal);
    if (!buffer) winFail(L"Cannot lock clipboard text");
    std::memcpy(buffer, text.c_str(), bytes);
    GlobalUnlock(entry.medium.value.hGlobal);
    object->entries.push_back(std::move(entry));
    return object;
}

void comPause(HANDLE event, DWORD milliseconds) noexcept {
    DWORD index = 0;
    CoWaitForMultipleHandles(0, milliseconds, 1, &event, &index);
}

struct PendingClipboard {
    enum class Phase { Temporary, Restored, Interrupted };
    ComPtr<ClipboardData> snapshot;
    ComPtr<ClipboardData> temporary;
    HWND owner = nullptr;
    DWORD sequence = 0;
    Phase phase = Phase::Temporary;

    void clear() noexcept {
        snapshot.Reset();
        temporary.Reset();
        owner = nullptr;
        sequence = 0;
        phase = Phase::Temporary;
    }
    void recordFailure() noexcept {
        if (OleIsCurrentClipboard(snapshot.Get()) == S_OK) phase = Phase::Restored;
        else if (OleIsCurrentClipboard(temporary.Get()) == S_OK) phase = Phase::Temporary;
        else {
            phase = Phase::Interrupted;
            sequence = GetClipboardSequenceNumber();
        }
    }
    bool stillOwned() const {
        if (GetClipboardOwner() != owner) return false;
        if (phase == Phase::Interrupted) {
            if (GetClipboardSequenceNumber() != sequence)
                fail(L"Clipboard changed after an interrupted restore. The original snapshot is retained; keep TalkText open.");
            return true;
        }
        auto* expected = phase == Phase::Restored ? snapshot.Get() : temporary.Get();
        if (OleIsCurrentClipboard(expected) != S_OK)
            fail(L"Clipboard ownership could not be verified. The original snapshot is retained; keep TalkText open and choose Restore clipboard.");
        return true;
    }
};

thread_local PendingClipboard pendingClipboard;
thread_local bool clipboardOperationActive = false;

struct ClipboardOperation {
    ClipboardOperation() noexcept { clipboardOperationActive = true; }
    ~ClipboardOperation() { clipboardOperationActive = false; }
};

struct ClipboardLock {
    explicit ClipboardLock(HWND owner) noexcept : opened(OpenClipboard(owner) != FALSE) {}
    ~ClipboardLock() { if (opened) CloseClipboard(); }
    bool opened;
};

std::wstring restoreClipboard() {
    auto& pending = pendingClipboard;
    if (!pending.snapshot) return {};
    pending.temporary->notifyReads(false);
    HANDLE pauseEvent = pending.temporary->readEvent();
    ResetEvent(pauseEvent);
    const ULONGLONG deadline = GetTickCount64() + 1000;
    do {
        if (!pending.stillOwned()) {
            pending.clear();
            return L"Clipboard changed during delivery. New clipboard contents were kept; check the pasted text.";
        }
        ClipboardLock lock(pending.owner);
        if (!lock.opened) {
            comPause(pauseEvent, 20);
            continue;
        }
        // Reopen the same OLE window so checking ownership and replacing data
        // share a clipboard lock. Each OLE operation can close that lock itself.
        if (!pending.stillOwned()) {
            pending.clear();
            return L"Clipboard changed during delivery. New clipboard contents were kept; check the pasted text.";
        }
        if (pending.phase != PendingClipboard::Phase::Restored) {
            const HRESULT restored = OleSetClipboard(pending.snapshot.Get());
            if (FAILED(restored)) {
                pending.recordFailure();
                return L"Could not restore the clipboard. The original snapshot is retained; keep TalkText open and choose Restore clipboard: " +
                       systemMessage(restored);
            }
            pending.phase = PendingClipboard::Phase::Restored;
            continue;
        }
        const HRESULT flushed = OleFlushClipboard();
        if (FAILED(flushed)) {
            pending.recordFailure();
            return L"Clipboard was restored but still depends on TalkText. Keep TalkText open and choose Restore clipboard: " +
                   systemMessage(flushed);
        }
        pending.clear();
        return {};
    } while (GetTickCount64() < deadline);
    return L"Another application kept the clipboard locked. The original snapshot is retained; keep TalkText open and choose Restore clipboard.";
}

DWORD integrityLevel(HANDLE process) {
    Handle token;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) winFail(L"Cannot inspect target permissions");
    token.reset(rawToken);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &bytes);
    if (!bytes) winFail(L"Cannot inspect process integrity level");
    std::vector<unsigned char> storage(bytes);
    if (!GetTokenInformation(token.get(), TokenIntegrityLevel, storage.data(), bytes, &bytes))
        winFail(L"Cannot inspect process integrity level");
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    if (!IsValidSid(label->Label.Sid)) fail(L"Process returned an invalid integrity identifier.");
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) fail(L"Process returned an empty integrity identifier.");
    return *GetSidSubAuthority(label->Label.Sid, count - 1);
}

bool modifiersHeld() noexcept {
    constexpr UINT modifiers[]{VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN};
    for (UINT key : modifiers) if (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) return true;
    return false;
}

struct InputTarget {
    HWND root = nullptr;
    HWND focus = nullptr;
    DWORD thread = 0;

    std::wstring check() const {
        const HWND foreground = GetForegroundWindow();
        if (!IsWindow(root) || !foreground || GetAncestor(foreground, GA_ROOT) != root)
            return L"The target window is no longer in front. Select its text field and try again; nothing else will be typed.";
        GUITHREADINFO state{sizeof(state)};
        if (!GetGUIThreadInfo(thread, &state)) return L"Cannot inspect the target text field.";
        if (focus && state.hwndFocus != focus)
            return L"The target text field changed. Delivery stopped to avoid typing into the wrong field.";
        if (state.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
            return L"Close the target application's menu before inserting text.";
        return {};
    }
};

InputTarget prepareTarget(HWND window) {
    if (!window || !IsWindow(window)) fail(L"The target window has closed. Select a text field and try again.");
    InputTarget target;
    target.root = GetAncestor(window, GA_ROOT);
    DWORD processId = 0;
    target.thread = GetWindowThreadProcessId(target.root, &processId);
    if (const auto error = target.check(); !error.empty()) fail(error);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process) fail(L"Cannot access the target application. It may be elevated or protected; use Clipboard only.");
    if (integrityLevel(process.get()) > integrityLevel(GetCurrentProcess()))
        fail(L"The target runs at a higher permission level. Run both apps at the same level, or use Clipboard only and paste manually.");
    GUITHREADINFO state{sizeof(state)};
    if (!GetGUIThreadInfo(target.thread, &state)) winFail(L"Cannot locate the target text field");
    target.focus = state.hwndFocus;
    if (modifiersHeld()) fail(L"Release Ctrl, Alt, Shift, and Windows keys before inserting text.");
    return target;
}

INPUT keyInput(WORD key, bool up, bool unicode = false) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    if (unicode) input.ki.wScan = key;
    else input.ki.wVk = key;
    input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (unicode ? KEYEVENTF_UNICODE : 0);
    return input;
}

std::wstring sendKeys(INPUT* inputs, UINT count) {
    SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent == count) return {};
    const DWORD error = GetLastError();
    // Release only the injected keys whose key-up event was not accepted.
    for (UINT index = 0; index < sent; ++index) {
        if (inputs[index].ki.dwFlags & KEYEVENTF_KEYUP) continue;
        bool released = false;
        for (UINT later = index + 1; later < sent; ++later) {
            if ((inputs[later].ki.dwFlags & KEYEVENTF_KEYUP) &&
                inputs[later].ki.wVk == inputs[index].ki.wVk &&
                inputs[later].ki.wScan == inputs[index].ki.wScan) released = true;
        }
        if (!released) {
            INPUT release = inputs[index];
            release.ki.dwFlags |= KEYEVENTF_KEYUP;
            SendInput(1, &release, sizeof(INPUT));
        }
    }
    std::wstring result = sent ? L"Windows accepted only part of the input. Do not retry blindly; check the target. " :
                                L"Windows blocked the input. ";
    result += L"Elevated or protected apps can reject simulated keys; use Clipboard only.";
    if (error) result += L" " + systemMessage(error);
    return result;
}

std::wstring typeText(const InputTarget& target, const std::wstring& text) {
    std::array<INPUT, 128> inputs{};
    size_t offset = 0;
    while (offset < text.size()) {
        if (auto error = target.check(); !error.empty()) return error;
        if (modifiersHeld()) return L"A modifier key was pressed during typing. Delivery stopped; check the partial text.";
        size_t characters = std::min<size_t>(inputs.size() / 2, text.size() - offset);
        if (offset + characters < text.size() && text[offset + characters - 1] >= 0xD800 &&
            text[offset + characters - 1] <= 0xDBFF) --characters;
        for (size_t index = 0; index < characters; ++index) {
            inputs[index * 2] = keyInput(text[offset + index], false, true);
            inputs[index * 2 + 1] = keyInput(text[offset + index], true, true);
        }
        if (auto error = sendKeys(inputs.data(), static_cast<UINT>(characters * 2)); !error.empty()) return error;
        offset += characters;
    }
    return {};
}

void validateText(const std::wstring& text) {
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t ch = text[index];
        if (!ch) fail(L"Text contains an embedded null character and cannot be delivered safely.");
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++index == text.size() || text[index] < 0xDC00 || text[index] > 0xDFFF)
                fail(L"Text contains invalid Unicode.");
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            fail(L"Text contains invalid Unicode.");
        }
    }
}

} // namespace

Settings loadSettings() {
    FileReader file(dataPath(L"settings.dat"));
    Settings settings;
    if (!file.exists()) return settings;
    file.header(settingsHeader);
    const uint32_t flags = file.number<uint32_t>();
    const uint32_t method = file.number<uint32_t>();
    const uint64_t historyLimit = file.number<uint64_t>();
    const uint32_t keyCount = file.number<uint32_t>();
    if ((flags & ~31u) || method > 2 || historyLimit > std::numeric_limits<size_t>::max() ||
        keyCount == 0 || keyCount >= 255 || keyCount > file.remaining() / sizeof(uint32_t))
        fail(L"Saved settings contain invalid values.");
    settings.startWithWindows = (flags & 1) != 0;
    settings.keepInTray = (flags & 2) != 0;
    settings.startHidden = (flags & 4) != 0;
    settings.pushToTalk = (flags & 8) != 0;
    settings.autoEnter = (flags & 16) != 0;
    settings.method = static_cast<InsertMethod>(method);
    settings.historyLimit = static_cast<size_t>(historyLimit);
    settings.hotkey.clear();
    for (uint32_t index = 0; index < keyCount; ++index) settings.hotkey.push_back(file.number<uint32_t>());
    settings.hotkey = normalizedChord(settings.hotkey);
    file.finish();
    return settings;
}

void saveSettings(const Settings& settings) {
    const auto chord = normalizedChord(settings.hotkey);
    const auto method = static_cast<uint32_t>(settings.method);
    if (method > 2) fail(L"Invalid text insertion method.");
    const uint32_t flags = (settings.startWithWindows ? 1u : 0u) | (settings.keepInTray ? 2u : 0u) |
        (settings.startHidden ? 4u : 0u) | (settings.pushToTalk ? 8u : 0u) | (settings.autoEnter ? 16u : 0u);
    AtomicFile file(dataPath(L"settings.dat"));
    file.write(settingsHeader.data(), settingsHeader.size());
    file.number(flags);
    file.number(method);
    file.number<uint64_t>(settings.historyLimit);
    file.number<uint32_t>(static_cast<uint32_t>(chord.size()));
    for (UINT key : chord) file.number<uint32_t>(key);
    file.commit();
}

std::vector<HistoryEntry> loadHistory() {
    FileReader file(dataPath(L"history.dat"));
    std::vector<HistoryEntry> history;
    if (!file.exists()) return history;
    file.header(historyHeader);
    const uint64_t count = file.number<uint64_t>();
    if (count > history.max_size() || count > file.remaining() / (2 * sizeof(uint64_t)))
        fail(L"Saved history contains an invalid entry count.");
    for (uint64_t index = 0; index < count; ++index) {
        HistoryEntry entry;
        entry.timestamp = file.string();
        entry.text = file.string();
        history.push_back(std::move(entry));
    }
    file.finish();
    return history;
}

void saveHistory(const std::vector<HistoryEntry>& history) {
    AtomicFile file(dataPath(L"history.dat"));
    file.write(historyHeader.data(), historyHeader.size());
    file.number<uint64_t>(history.size());
    for (const auto& entry : history) {
        file.string(entry.timestamp);
        file.string(entry.text);
    }
    file.commit();
}

void setStartup(bool enabled) {
    constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    LSTATUS result = enabled ? RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0,
                                              KEY_SET_VALUE, nullptr, &key, nullptr) :
                               RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key);
    if (!enabled && result == ERROR_FILE_NOT_FOUND) return;
    if (result != ERROR_SUCCESS) winFail(L"Cannot update Windows startup registration", result);
    if (enabled) {
        std::wstring command;
        try {
            command = L"\"" + executablePath().wstring() + L"\" --startup";
        } catch (...) {
            RegCloseKey(key);
            throw;
        }
        if (command.size() > 260) {
            RegCloseKey(key);
            fail(L"Windows Run commands are limited to 260 characters. Move TalkText to a shorter path.");
        }
        result = RegSetValueExW(key, L"TalkText", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"TalkText");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) winFail(L"Cannot update Windows startup registration", result);
}

std::filesystem::path appDirectory() { return executablePath().parent_path(); }

std::wstring keyName(UINT key) {
    key = normalizeKey(key);
    switch (key) {
    case VK_CONTROL: return L"Ctrl";
    case VK_SHIFT: return L"Shift";
    case VK_MENU: return L"Alt";
    case VK_LWIN: return L"Win";
    case VK_LBUTTON: return L"Mouse Left";
    case VK_RBUTTON: return L"Mouse Right";
    case VK_MBUTTON: return L"Mouse Middle";
    case VK_XBUTTON1: return L"Mouse 4";
    case VK_XBUTTON2: return L"Mouse 5";
    case VK_SPACE: return L"Space";
    default: break;
    }
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    LONG packed = static_cast<LONG>((scan & 0xFF) << 16);
    if (scan & 0xFF00) packed |= 1 << 24;
    wchar_t name[128]{};
    if (scan && GetKeyNameTextW(packed, name, static_cast<int>(std::size(name)))) return name;
    wchar_t fallback[24]{};
    swprintf_s(fallback, L"Key 0x%02X", key);
    return fallback;
}

std::wstring chordName(const std::vector<UINT>& keys) {
    std::wstring result;
    for (UINT key : keys) {
        if (!result.empty()) result += L" + ";
        result += keyName(key);
    }
    return result;
}

bool hasPendingClipboard() {
    return pendingClipboard.snapshot != nullptr;
}

std::wstring restorePendingClipboard() {
    if (!hasPendingClipboard()) return {};
    if (clipboardOperationActive)
        return L"Clipboard delivery or recovery is still in progress. Keep TalkText open.";
    ClipboardOperation operation;
    try {
        return restoreClipboard();
    } catch (const std::exception& exception) {
        return widen(exception.what());
    }
}

std::wstring insertText(HWND window, const std::wstring& text, InsertMethod method, bool autoEnter) {
    try {
        if (clipboardOperationActive)
            return L"Text delivery or clipboard recovery is already in progress.";
        const std::wstring recovery = restorePendingClipboard();
        if (hasPendingClipboard()) return recovery;
        ClipboardOperation operation;
        if (text.empty()) return {};
        validateText(text);
        if (method != InsertMethod::Paste && method != InsertMethod::Type && method != InsertMethod::ClipboardOnly)
            return L"Invalid text insertion method.";
        if (method == InsertMethod::ClipboardOnly) {
            auto object = textClipboard(text);
            requireHr(OleSetClipboard(object.Get()), L"Cannot replace the clipboard");
            requireHr(OleFlushClipboard(), L"Text is on the clipboard, but TalkText must remain open to retain it");
            return {};
        }
        const InputTarget target = prepareTarget(window);
        std::wstring error;
        if (method == InsertMethod::Type) {
            error = typeText(target, text);
        } else {
            auto object = textClipboard(text);
            DWORD sequence = 0;
            auto snapshot = captureClipboard(sequence);
            if (auto changed = target.check(); !changed.empty()) return changed;
            if (modifiersHeld()) return L"Release modifier keys before pasting.";
            if (sequence != GetClipboardSequenceNumber())
                return L"Clipboard changed before delivery. Nothing was inserted; try again.";
            // Retain recovery ownership before any call that can empty the clipboard.
            auto& pending = pendingClipboard;
            pending.snapshot = snapshot;
            pending.temporary = object;
            const HWND previousOwner = GetClipboardOwner();
            const HRESULT installed = OleSetClipboard(object.Get());
            pending.owner = GetClipboardOwner();
            pending.phase = PendingClipboard::Phase::Temporary;
            if (FAILED(installed)) {
                if ((pending.owner == previousOwner && GetClipboardSequenceNumber() == sequence) ||
                    (pending.owner && GetWindowThreadProcessId(pending.owner, nullptr) != GetCurrentThreadId())) {
                    pending.clear();
                } else {
                    pending.recordFailure();
                }
                requireHr(installed, L"Cannot place temporary text on the clipboard");
            }
            object->notifyReads(true);
            try {
                error = target.check();
                if (error.empty()) {
                    std::array<INPUT, 4> paste{keyInput(VK_CONTROL, false), keyInput('V', false),
                                             keyInput('V', true), keyInput(VK_CONTROL, true)};
                    error = sendKeys(paste.data(), static_cast<UINT>(paste.size()));
                }
                if (error.empty()) {
                    HANDLE event = object->readEvent();
                    DWORD index = 0;
                    const HRESULT read = CoWaitForMultipleHandles(0, 1500, 1, &event, &index);
                    if (read != S_OK)
                        error = L"The target did not request the clipboard in time. Check the target; use Unicode typing if it does not support Ctrl+V.";
                }
            } catch (const std::exception& exception) {
                error = widen(exception.what());
            }
            object->notifyReads(false);
            ResetEvent(object->readEvent());
            // Let the consumer finish reading its rendered handle before restoring.
            comPause(object->readEvent(), 80);
            const std::wstring restoration = restoreClipboard();
            if (!restoration.empty()) {
                if (!error.empty()) error += L" ";
                error += restoration;
            }
        }
        if (!error.empty() || !autoEnter) return error;
        if (auto changed = target.check(); !changed.empty()) return changed;
        if (modifiersHeld()) return L"Text was delivered, but Enter was not sent because a modifier key is held.";
        std::array<INPUT, 2> enter{keyInput(VK_RETURN, false), keyInput(VK_RETURN, true)};
        return sendKeys(enter.data(), static_cast<UINT>(enter.size()));
    } catch (const std::exception& exception) {
        return widen(exception.what());
    }
}

struct Hotkeys::Impl {
    HWND receiver = nullptr;
    HHOOK keyboard = nullptr;
    HHOOK mouse = nullptr;
    DWORD thread = 0;
    std::vector<UINT> chord;
    std::array<bool, 256> pressed{};
    std::array<bool, 256> capturedPress{};
    bool capturing = false;
    bool active = false;
    bool blocked = false;
    static thread_local Impl* current;

    ~Impl() {
        if (keyboard) UnhookWindowsHookEx(keyboard);
        if (mouse) UnhookWindowsHookEx(mouse);
        if (current == this) current = nullptr;
    }
    void requireThread() const {
        if (thread && thread != GetCurrentThreadId())
            fail(L"Hotkeys must be changed on the installing message-loop thread.");
    }
    bool down(UINT key) const noexcept {
        switch (normalizeKey(key)) {
        case VK_CONTROL: return pressed[VK_LCONTROL] || pressed[VK_RCONTROL];
        case VK_SHIFT: return pressed[VK_LSHIFT] || pressed[VK_RSHIFT];
        case VK_MENU: return pressed[VK_LMENU] || pressed[VK_RMENU];
        case VK_LWIN: return pressed[VK_LWIN] || pressed[VK_RWIN];
        default: return key < pressed.size() && pressed[key];
        }
    }
    bool anyDown() const noexcept {
        return std::any_of(chord.begin(), chord.end(), [this](UINT key) { return down(key); });
    }
    void refresh() noexcept {
        for (UINT key = 0; key < pressed.size(); ++key) {
            if (!capturedPress[key])
                pressed[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        }
        pressed[VK_CONTROL] = pressed[VK_SHIFT] = pressed[VK_MENU] = false;
        blocked = anyDown();
    }
    void event(UINT key, bool isDown) noexcept {
        if (key == 0 || key >= pressed.size()) return;
        const bool wasDown = down(normalizeKey(key));
        pressed[key] = isDown;
        const bool fresh = isDown && !wasDown;
        if (key == VK_ESCAPE && fresh) PostMessageW(receiver, WM_HOTKEY_EVENT, 3, 0);
        if (capturing) {
            if (fresh && key != VK_ESCAPE && key != VK_PACKET && key != VK_PROCESSKEY)
                PostMessageW(receiver, WM_HOTKEY_CAPTURE, normalizeKey(key), 0);
            return;
        }
        if (blocked) {
            if (!anyDown()) blocked = false;
            return;
        }
        const bool nowActive = !chord.empty() &&
            std::all_of(chord.begin(), chord.end(), [this](UINT candidate) { return down(candidate); });
        if (nowActive != active) {
            active = nowActive;
            PostMessageW(receiver, WM_HOTKEY_EVENT, active ? 1 : 2, 0);
        }
    }
    static LRESULT CALLBACK keyboardHook(int code, WPARAM message, LPARAM parameter) {
        if (code == HC_ACTION && current) {
            const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(parameter);
            if (!(event.flags & LLKHF_INJECTED)) {
                UINT key = event.vkCode;
                if (key == VK_CONTROL) key = event.flags & LLKHF_EXTENDED ? VK_RCONTROL : VK_LCONTROL;
                if (key == VK_MENU) key = event.flags & LLKHF_EXTENDED ? VK_RMENU : VK_LMENU;
                if (key == VK_SHIFT) {
                    key = MapVirtualKeyW(event.scanCode, MAPVK_VSC_TO_VK_EX);
                    if (key != VK_LSHIFT && key != VK_RSHIFT) key = VK_LSHIFT;
                }
                if (key < current->pressed.size()) {
                    const bool isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                    const bool consume = current->capturedPress[key] || (current->capturing && isDown);
                    if (isDown && current->capturing) current->capturedPress[key] = true;
                    if (!isDown) current->capturedPress[key] = false;
                    current->event(key, isDown);
                    if (consume) return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, parameter);
    }
    static LRESULT CALLBACK mouseHook(int code, WPARAM message, LPARAM parameter) {
        if (code == HC_ACTION && current) {
            const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(parameter);
            if (!(event.flags & LLMHF_INJECTED)) {
                UINT key = 0;
                bool isDown = false;
                switch (message) {
                case WM_LBUTTONDOWN: key = VK_LBUTTON; isDown = true; break;
                case WM_LBUTTONUP: key = VK_LBUTTON; break;
                case WM_RBUTTONDOWN: key = VK_RBUTTON; isDown = true; break;
                case WM_RBUTTONUP: key = VK_RBUTTON; break;
                case WM_MBUTTONDOWN: key = VK_MBUTTON; isDown = true; break;
                case WM_MBUTTONUP: key = VK_MBUTTON; break;
                case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                    key = HIWORD(event.mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                    isDown = message == WM_XBUTTONDOWN;
                    break;
                default: break;
                }
                if (key) current->event(key, isDown);
            }
        }
        return CallNextHookEx(nullptr, code, message, parameter);
    }
};

thread_local Hotkeys::Impl* Hotkeys::Impl::current = nullptr;

Hotkeys::Hotkeys() : impl_(std::make_unique<Impl>()) {}
Hotkeys::~Hotkeys() = default;

void Hotkeys::install(HWND receiver, const std::vector<UINT>& keys) {
    impl_->requireThread();
    if (!IsWindow(receiver) || GetWindowThreadProcessId(receiver, nullptr) != GetCurrentThreadId())
        fail(L"Install hotkeys on the receiver window's message-loop thread.");
    if (Impl::current && Impl::current != impl_.get())
        fail(L"Only one Hotkeys instance can be installed on a thread.");
    auto chord = normalizedChord(keys);
    if (impl_->keyboard || impl_->mouse) {
        impl_->receiver = receiver;
        setChord(chord);
        return;
    }
    impl_->receiver = receiver;
    impl_->thread = GetCurrentThreadId();
    impl_->chord = std::move(chord);
    impl_->refresh();
    Impl::current = impl_.get();
    impl_->keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, Impl::keyboardHook, GetModuleHandleW(nullptr), 0);
    if (!impl_->keyboard) {
        const DWORD error = GetLastError();
        Impl::current = nullptr;
        winFail(L"Cannot install global keyboard hook", error);
    }
    impl_->mouse = SetWindowsHookExW(WH_MOUSE_LL, Impl::mouseHook, GetModuleHandleW(nullptr), 0);
    if (!impl_->mouse) {
        const DWORD error = GetLastError();
        UnhookWindowsHookEx(impl_->keyboard);
        impl_->keyboard = nullptr;
        Impl::current = nullptr;
        winFail(L"Cannot install global mouse hook", error);
    }
}

void Hotkeys::uninstall() {
    impl_->requireThread();
    if (impl_->keyboard && !UnhookWindowsHookEx(impl_->keyboard))
        winFail(L"Cannot remove keyboard hook");
    impl_->keyboard = nullptr;
    if (impl_->mouse && !UnhookWindowsHookEx(impl_->mouse))
        winFail(L"Cannot remove mouse hook");
    impl_->mouse = nullptr;
    if (Impl::current == impl_.get()) Impl::current = nullptr;
    impl_->receiver = nullptr;
    impl_->thread = 0;
    impl_->capturing = false;
    impl_->active = false;
    impl_->blocked = false;
    impl_->pressed.fill(false);
    impl_->capturedPress.fill(false);
}

void Hotkeys::setChord(const std::vector<UINT>& keys) {
    impl_->requireThread();
    auto chord = normalizedChord(keys);
    if (impl_->active) PostMessageW(impl_->receiver, WM_HOTKEY_EVENT, 2, 0);
    impl_->active = false;
    impl_->chord = std::move(chord);
    impl_->refresh();
}

void Hotkeys::capture(bool enabled) {
    impl_->requireThread();
    if (impl_->capturing == enabled) return;
    if (impl_->active) PostMessageW(impl_->receiver, WM_HOTKEY_EVENT, 2, 0);
    impl_->active = false;
    impl_->capturing = enabled;
    impl_->refresh();
}

} // namespace talktext
