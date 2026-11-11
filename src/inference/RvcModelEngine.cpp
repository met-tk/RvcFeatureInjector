#include "RvcModelEngine.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace rvc_vst {

namespace {

constexpr uint32_t kCmdInit       = 1;
constexpr uint32_t kCmdLoadSlot   = 2;
constexpr uint32_t kCmdClearSlot  = 4;
constexpr uint32_t kCmdSynthChunk = 5;
constexpr uint32_t kCmdShutdown   = 99;

constexpr uint32_t kMsgProgress    = 10;
constexpr uint32_t kMsgStatusOk    = 20;
constexpr uint32_t kMsgSlotInfo    = 21;
constexpr uint32_t kMsgSynthResult = 22;
constexpr uint32_t kMsgError       = 90;

#pragma pack(push, 1)
struct PipePacketHeader {
    char magic[4];
    uint32_t typeOrCmd;
    uint32_t payloadLen;
};

struct DaemonSlotInfoWire {
    uint32_t slotIdx;
    uint32_t isLoaded;
    uint32_t hasPth;
    uint32_t hasIndex;
    uint32_t sr;
    uint32_t version;
    uint32_t vectorCount;
    uint32_t featureDim;
    char modelName[64];
    char filePath[512];
    char archTag[64];
    float baseFormantDb[kRvcFeatureDim];
    char indexPath[512];
};

struct SynthRequestWireHeader {
    float abMorph;
    float injectDepth;
    float pitchShiftSt;
    float f0FilterRadius;
    float indexRate;
    float protect;
    float slotAIndexRate;
    float slotBIndexRate;
    float rmsMixRate;
    uint32_t targetSr;
    uint32_t numSamples16k;
    uint32_t numAutoFrames;
};

struct SynthResultWireHeader {
    float detectedF0Hz;
    float elapsedMs;
    uint32_t numOutSamples;
    float dryEnv64[64];
    float slotAEnv64[64];
    float slotBEnv64[64];
    float outEnv64[64];
};
#pragma pack(pop)

std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring res(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), res.data(), len);
    return res;
}

fs::path resolveManagedStateRoot(const std::wstring& configured, const fs::path& localAppData) {
    constexpr wchar_t token[] = L"%LOCALAPPDATA%";
    constexpr size_t tokenLength = std::size(token) - 1;
    fs::path result(configured);
    if (configured.compare(0, tokenLength, token) == 0) {
        if (configured.size() <= tokenLength + 1 ||
            (configured[tokenLength] != L'\\' && configured[tokenLength] != L'/') ||
            !localAppData.is_absolute()) return {};
        const fs::path suffix(configured.substr(tokenLength + 1));
        if (suffix.has_root_path()) return {};
        for (const auto& component : suffix) {
            if (component == L".." || component == L".") return {};
        }
        result = localAppData / suffix;
    }
    if (!result.is_absolute() || result == result.root_path() ||
        result.wstring().find(L'"') != std::wstring::npos) return {};
    return result;
}

fs::path currentUserLocalAppData() {
    PWSTR directory = nullptr;
    // Resolve in the host's user context, never from the elevated installer's environment.
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &directory);
    const fs::path path = SUCCEEDED(result) && directory ? fs::path(directory) : fs::path{};
    CoTaskMemFree(directory);
    return path;
}

bool ensureManagedStateMarker(const fs::path& stateRoot) {
    std::error_code error;
    fs::create_directories(stateRoot, error);
    if (error) return false;
    const fs::path marker = stateRoot / L".taketo-rvc-state";
    constexpr char contents[] = "TAKETO-RVCFEATUREINJECTOR-STATE-V1\n";
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const BOOL ok = WriteFile(file, contents, sizeof(contents) - 1, &written, nullptr);
        CloseHandle(file);
        return ok && written == sizeof(contents) - 1;
    }
    if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    // Another plugin instance may still be committing its new marker.
    for (int attempt = 0; attempt < 20; ++attempt) {
        file = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
        Sleep(5);
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    char found[sizeof(contents)]{};
    DWORD read = 0;
    const bool valid = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ReadFile(file, found, sizeof(found), &read, nullptr) &&
        read == sizeof(contents) - 1 && std::memcmp(found, contents, sizeof(contents) - 1) == 0;
    CloseHandle(file);
    return valid;
}

std::wstring quoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        quoted.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted += character;
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

bool createAsyncPipe(HANDLE& parent, HANDLE& child, bool parentWrites, SECURITY_ATTRIBUTES& childSecurity) {
    static std::atomic<uint64_t> serial{0};
    const std::wstring name = L"\\\\.\\pipe\\RvcFeatureInjector_" + std::to_wstring(GetCurrentProcessId()) +
                              L"_" + std::to_wstring(serial.fetch_add(1));
    parent = CreateNamedPipeW(name.c_str(), (parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
                             FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                             PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (parent == INVALID_HANDLE_VALUE) { parent = nullptr; return false; }
    child = CreateFileW(name.c_str(), parentWrites ? GENERIC_READ : GENERIC_WRITE, 0, &childSecurity,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        child = nullptr; CloseHandle(parent); parent = nullptr; return false;
    }
    OVERLAPPED connection{};
    connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connection.hEvent) {
        CloseHandle(child); CloseHandle(parent); child = parent = nullptr; return false;
    }
    const BOOL connected = ConnectNamedPipe(parent, &connection);
    const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error != ERROR_PIPE_CONNECTED) {
        if (error == ERROR_IO_PENDING) {
            CancelIoEx(parent, &connection);
            DWORD ignored = 0;
            GetOverlappedResult(parent, &connection, &ignored, TRUE);
        }
        CloseHandle(connection.hEvent);
        CloseHandle(child); CloseHandle(parent); child = parent = nullptr; return false;
    }
    CloseHandle(connection.hEvent);
    return true;
}

} // namespace

#ifdef RVC_ENGINE_REGRESSION
bool verifyManagedStateRootResolutionForTest(const std::wstring& fixtureRoot) {
    const fs::path root(fixtureRoot);
    const fs::path suffix = L"Taketo\\RvcFeatureInjector\\state";
    const auto tokenPath = L"%LOCALAPPDATA%\\Taketo\\RvcFeatureInjector\\state";
    const fs::path actualUser = currentUserLocalAppData();
    return resolveManagedStateRoot(tokenPath, root) == root / suffix &&
        !actualUser.empty() && resolveManagedStateRoot(tokenPath, actualUser) == actualUser / suffix &&
        resolveManagedStateRoot(root.wstring(), {}) == root &&
        resolveManagedStateRoot(L"%LOCALAPPDATA%\\..\\other", root).empty() &&
        resolveManagedStateRoot(L"%LOCALAPPDATA%\\\\other", root).empty() &&
        resolveManagedStateRoot(L"%LOCALAPPDATA%", root).empty() &&
        resolveManagedStateRoot(L"%LOCALAPPDATA%other", root).empty() &&
        resolveManagedStateRoot(L"%localappdata%\\state", root).empty() &&
        resolveManagedStateRoot(L"%TEMP%\\state", root).empty() &&
        resolveManagedStateRoot(L"%LOCALAPPDATA%\\state", {}).empty();
}
#endif

enum class PendingSlotAction : uint8_t {
    kNone = 0,
    kLoadFile = 1,
    kClear = 2
};

struct SlotRuntimeData {
    RvcModelSlotInfo info;
    std::wstring pendingFilePath;
    std::wstring pendingIndexPath;
    bool explicitIndex = false;
    uint64_t selectionRevision = 0;
    PendingSlotAction pendingAction = PendingSlotAction::kNone;
};

struct RvcModelEngine::Impl {
    mutable std::mutex stateMutex;
    mutable std::mutex ipcMutex;
    mutable std::mutex statusMutex;
    mutable std::mutex cancelMutex;

    std::string loadingStatusText = "READY // RVC-V2 CUDA ENGINE STANDBY";

    fs::path pythonExePath;
    fs::path daemonScriptPath;
    fs::path runtimeRootDir;
    fs::path managedStateRootDir;

    // Persistent Python CUDA Daemon Process & Pipe Handles
    HANDLE hChildProcess = nullptr;
    HANDLE hChildThread = nullptr;
    HANDLE hPipeWriteStdin = nullptr;
    HANDLE hPipeReadStdout = nullptr;
    HANDLE hDaemonLog = nullptr;   // Child stderr sink (GUI hosts have no valid stderr)
    HANDLE hKillJob = nullptr;     // Job object: kills python.exe if the host process dies
    bool daemonSpawnAttempted = false;
    HANDLE hCancelEvent = nullptr;
    std::atomic<uint64_t> cancelGeneration{0};
    std::atomic<bool> restartRequested{false};
    bool cancellationLatched = false;
    ULONGLONG operationDeadline = 0;
    std::string ioFailureText;

    std::array<SlotRuntimeData, 2> slots;
    std::array<float, kRvcFeatureDim> arenaInMel{};

    LARGE_INTEGER qpcFreq{};

    Impl() {
        QueryPerformanceFrequency(&qpcFreq);
        hCancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~Impl() {
        cancelWork();
        shutdownDaemon();
        if (hCancelEvent) CloseHandle(hCancelEvent);
        if (hKillJob) {
            CloseHandle(hKillJob);
            hKillJob = nullptr;
        }
    }

    void cancelWork() noexcept {
        std::lock_guard<std::mutex> lock(cancelMutex);
        cancelGeneration.fetch_add(1, std::memory_order_release);
        restartRequested.store(true, std::memory_order_release);
        cancellationLatched = true;
        if (hCancelEvent) SetEvent(hCancelEvent);
    }

    // The IPC mutex serializes operations; cancellation never waits for that mutex.
    bool beginOperation(uint64_t generation) {
        std::lock_guard<std::mutex> lock(cancelMutex);
        if (!hCancelEvent || cancellationLatched || cancelGeneration.load(std::memory_order_acquire) != generation) return false;
        ResetEvent(hCancelEvent);
        DWORD timeoutMs = 300000;
        wchar_t configured[32]{};
        const DWORD length = GetEnvironmentVariableW(L"RVC_IPC_TIMEOUT_MS", configured, 32);
        if (length > 0 && length < 32) {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(configured, &end, 10);
            if (end && *end == L'\0' && value >= 100 && value <= 1800000) timeoutMs = value;
        }
        operationDeadline = GetTickCount64() + timeoutMs;
        ioFailureText.clear();
        return true;
    }

    bool transferPipe(HANDLE pipe, void* data, size_t totalBytes, bool writing) {
        if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;
        auto* ptr = static_cast<uint8_t*>(data);
        HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ioEvent) return false;
        bool success = true;
        while (totalBytes > 0) {
            if (WaitForSingleObject(hCancelEvent, 0) == WAIT_OBJECT_0) {
                ioFailureText = "RVC OPERATION CANCELLED"; success = false; break;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= operationDeadline) {
                ioFailureText = "RVC DAEMON IPC TIMED OUT"; success = false; break;
            }
            OVERLAPPED operation{};
            ResetEvent(ioEvent);
            operation.hEvent = ioEvent;
            DWORD transferred = 0;
            const DWORD count = static_cast<DWORD>(std::min<size_t>(totalBytes, 256 * 1024));
            BOOL done = writing ? WriteFile(pipe, ptr, count, &transferred, &operation) :
                                  ReadFile(pipe, ptr, count, &transferred, &operation);
            if (!done && GetLastError() == ERROR_IO_PENDING) {
                HANDLE waits[] = {hCancelEvent, ioEvent};
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE,
                    static_cast<DWORD>(std::min<ULONGLONG>(operationDeadline - now, MAXDWORD - 1)));
                if (wait == WAIT_OBJECT_0 + 1) {
                    done = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
                } else {
                    ioFailureText = wait == WAIT_TIMEOUT ? "RVC DAEMON IPC TIMED OUT" : "RVC OPERATION CANCELLED";
                    CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                    done = FALSE;
                }
            }
            if (!done || transferred == 0) { success = false; break; }
            ptr += transferred;
            totalBytes -= transferred;
        }
        CloseHandle(ioEvent);
        if (!success && ioFailureText.empty()) ioFailureText = "RVC DAEMON PIPE DISCONNECTED";
        return success;
    }

    bool writeExactPipe(HANDLE pipe, const void* data, size_t count) {
        return transferPipe(pipe, const_cast<void*>(data), count, true);
    }
    bool readExactPipe(HANDLE pipe, void* data, size_t count) {
        return transferPipe(pipe, data, count, false);
    }

    void setStatus(std::atomic<float>& progAtom, float progress, const std::string& text) {
        progAtom.store(std::clamp(progress, 0.0f, 1.0f), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(statusMutex);
        loadingStatusText = text;
    }

    void shutdownDaemon() {
        std::lock_guard<std::mutex> lock(ipcMutex);
        shutdownDaemonLocked();
    }

    // Caller must hold ipcMutex.
    void shutdownDaemonLocked() {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            for (auto& slot : slots) {
                if (!slot.info.isLoaded) continue;
                if (slot.pendingAction == PendingSlotAction::kNone && !slot.info.filePath.empty()) {
                    slot.pendingFilePath = utf8ToWide(slot.info.filePath);
                    slot.pendingIndexPath = utf8ToWide(slot.info.indexPath);
                    slot.explicitIndex = true;
                    slot.pendingAction = PendingSlotAction::kLoadFile;
                }
                slot.info.isLoaded = false;
                slot.info.hasOnnxSession = false;
                slot.info.hasFeatureIndex = false;
            }
        }
        // A dead or unresponsive child must never block shutdown on a pipe write.
        if (hChildProcess) {
            TerminateProcess(hChildProcess, 0);
            WaitForSingleObject(hChildProcess, 200);
        }
        if (hPipeWriteStdin && hPipeWriteStdin != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipeWriteStdin);
            hPipeWriteStdin = nullptr;
        }
        if (hPipeReadStdout && hPipeReadStdout != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipeReadStdout);
            hPipeReadStdout = nullptr;
        }
        if (hChildProcess) {
            CloseHandle(hChildProcess);
            hChildProcess = nullptr;
        }
        if (hChildThread) {
            CloseHandle(hChildThread);
            hChildThread = nullptr;
        }
        if (hDaemonLog) {
            CloseHandle(hDaemonLog);
            hDaemonLog = nullptr;
        }
    }

    void applyWireSlotInfo(const DaemonSlotInfoWire& wire, uint64_t selectionRevision,
                           const std::wstring& requestedIndex) {
        if (wire.slotIdx > 1) return;
        std::lock_guard<std::mutex> lock(stateMutex);
        auto& slot = slots[wire.slotIdx];
        // Do not overwrite slot state if a newer user action (e.g. CLEAR or LOAD) was queued concurrently
        if (slot.selectionRevision != selectionRevision) {
            return;
        }
        slot.pendingAction = PendingSlotAction::kNone;
        const int32_t savedOctave = slot.info.targetOctaveSemitones;
        slot.info.isLoaded = (wire.isLoaded != 0);
        slot.info.hasOnnxSession = (wire.hasPth != 0);
        slot.info.hasFeatureIndex = (wire.hasIndex != 0);
        slot.info.sampleRate = wire.sr;
        slot.info.rvcVersion = wire.version;
        slot.info.centroidCount = wire.vectorCount;
        slot.info.featureDim = wire.featureDim;
        slot.info.targetOctaveSemitones = savedOctave;

        char safeName[65]{};
        std::memcpy(safeName, wire.modelName, 64);
        slot.info.modelName = safeName;

        char safePath[513]{};
        std::memcpy(safePath, wire.filePath, 512);
        slot.info.filePath = safePath;
        char safeIndex[513]{};
        std::memcpy(safeIndex, wire.indexPath, 512);
        slot.info.indexPath = wire.hasIndex ? (safeIndex[0] ? std::string(safeIndex) : wideToUtf8(requestedIndex)) : "";
        slot.pendingFilePath = utf8ToWide(slot.info.filePath);
        slot.pendingIndexPath = utf8ToWide(slot.info.indexPath);
        slot.explicitIndex = true;

        char safeArch[65]{};
        std::memcpy(safeArch, wire.archTag, 64);
        slot.info.archTag = safeArch;

        for (size_t i = 0; i < kRvcFeatureDim; ++i) {
            const float v = wire.baseFormantDb[i];
            slot.info.baseFormantDb[i] = std::isfinite(v) ? std::clamp(v, -24.0f, 24.0f) : 0.0f;
            slot.info.speakerEmbedding[i] = slot.info.baseFormantDb[i] / 12.0f;
        }
    }

    bool receiveDaemonResponse(
        uint32_t expectedMsgType,
        std::vector<uint8_t>& outPayload,
        std::atomic<float>& progAtom,
        const std::function<void(float, const std::string&)>& onProgress) {

        while (true) {
            PipePacketHeader hdr{};
            if (!readExactPipe(hPipeReadStdout, &hdr, sizeof(hdr))) {
                setStatus(progAtom, 1.0f, "ERR: " + ioFailureText);
                return false;
            }
            if (std::memcmp(hdr.magic, "RVCR", 4) != 0 || hdr.payloadLen > 64 * 1024 * 1024) {
                ioFailureText = "INVALID RVC DAEMON RESPONSE";
                setStatus(progAtom, 1.0f, "ERR: " + ioFailureText);
                return false;
            }

            std::vector<uint8_t> payload(hdr.payloadLen);
            if (hdr.payloadLen > 0 && !readExactPipe(hPipeReadStdout, payload.data(), hdr.payloadLen)) {
                setStatus(progAtom, 1.0f, "ERR: " + ioFailureText);
                return false;
            }

            if (hdr.typeOrCmd == kMsgProgress && payload.size() >= 8) {
                float p = 0.0f;
                uint32_t msgLen = 0;
                std::memcpy(&p, payload.data(), 4);
                std::memcpy(&msgLen, payload.data() + 4, 4);
                msgLen = std::min<uint32_t>(msgLen, static_cast<uint32_t>(payload.size() - 8));
                std::string msg(reinterpret_cast<const char*>(payload.data() + 8), msgLen);
                setStatus(progAtom, p, msg);
                if (onProgress) {
                    onProgress(p, msg);
                }
                continue;
            }

            if (hdr.typeOrCmd == kMsgError) {
                std::string errText = "DAEMON ERROR";
                if (payload.size() >= 4) {
                    uint32_t errLen = 0;
                    std::memcpy(&errLen, payload.data(), 4);
                    errLen = std::min<uint32_t>(errLen, static_cast<uint32_t>(payload.size() - 4));
                    errText.assign(reinterpret_cast<const char*>(payload.data() + 4), errLen);
                }
                setStatus(progAtom, 1.0f, "ERR: " + errText);
                return false;
            }

            if (hdr.typeOrCmd == expectedMsgType) {
                outPayload = std::move(payload);
                return true;
            }
        }
    }

    bool ensureDaemonStartedAndSynced(
        uint64_t generation,
        std::atomic<bool>& readyAtom,
        std::atomic<bool>& busyAtom,
        std::atomic<float>& progAtom,
        const std::function<void(float, const std::string&)>& onProgress = nullptr) {

        std::lock_guard<std::mutex> ipcLock(ipcMutex);
        if (!beginOperation(generation)) return false;
        if (restartRequested.exchange(false, std::memory_order_acq_rel)) {
            shutdownDaemonLocked();
            readyAtom.store(false, std::memory_order_release);
        }

        // 1. Check if child process is alive
        if (hChildProcess) {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(hChildProcess, &exitCode) || exitCode != STILL_ACTIVE) {
                shutdownDaemonLocked();
                readyAtom.store(false, std::memory_order_release);
            }
        }

        // 2. Spawn daemon if not yet running
        if (!hChildProcess) {
            if (pythonExePath.empty() || daemonScriptPath.empty() ||
                !fs::exists(pythonExePath) || !fs::exists(daemonScriptPath)) {
                setStatus(progAtom, 1.0f, "ERR: RVC RUNTIME NOT FOUND; SET RVC_RUNTIME_ROOT OR Resources/runtime-root.txt");
                return false;
            }
            if (pythonExePath.wstring().find(L'"') != std::wstring::npos ||
                daemonScriptPath.wstring().find(L'"') != std::wstring::npos) {
                return false;
            }
            if (!managedStateRootDir.empty() && !ensureManagedStateMarker(managedStateRootDir)) {
                setStatus(progAtom, 1.0f, "ERR: CANNOT CREATE MANAGED STATE OR STATE OWNERSHIP MARKER IS INVALID");
                return false;
            }

            busyAtom.store(true, std::memory_order_release);
            setStatus(progAtom, 0.02f, "STARTING RVC CUDA NEURAL ENGINE...");

            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            sa.lpSecurityDescriptor = nullptr;

            HANDLE hStdinRd = nullptr, hStdinWr = nullptr;
            HANDLE hStdoutRd = nullptr, hStdoutWr = nullptr;

            if (!createAsyncPipe(hStdinWr, hStdinRd, true, sa) ||
                !createAsyncPipe(hStdoutRd, hStdoutWr, false, sa)) {
                if (hStdinRd) CloseHandle(hStdinRd);
                if (hStdinWr) CloseHandle(hStdinWr);
                if (hStdoutRd) CloseHandle(hStdoutRd);
                if (hStdoutWr) CloseHandle(hStdoutWr);
                busyAtom.store(false, std::memory_order_release);
                return false;
            }

            SetHandleInformation(hStdinWr, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);

            // Installed copies keep diagnostics with their uninstallable state; portable
            // copies retain the existing temporary-log behavior.
            if (hDaemonLog) { CloseHandle(hDaemonLog); hDaemonLog = nullptr; }
            {
                fs::path logDir;
                if (!managedStateRootDir.empty()) {
                    std::error_code error;
                    logDir = managedStateRootDir / L"logs";
                    fs::create_directories(logDir, error);
                    if (error) logDir.clear();
                } else {
                    wchar_t tempDir[MAX_PATH + 1]{};
                    const DWORD n = GetTempPathW(MAX_PATH, tempDir);
                    if (n > 0 && n < MAX_PATH) logDir = tempDir;
                }
                if (!logDir.empty()) {
                    wchar_t logName[96]{};
                    swprintf_s(logName, L"RvcFeatureInjector_daemon_%lu_%p.log",
                               static_cast<unsigned long>(GetCurrentProcessId()), static_cast<void*>(this));
                    const auto logPath = logDir / logName;
                    hDaemonLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                }
                if (!hDaemonLog || hDaemonLog == INVALID_HANDLE_VALUE) {
                    hDaemonLog = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             &sa, OPEN_EXISTING, 0, nullptr);
                }
                if (hDaemonLog == INVALID_HANDLE_VALUE) hDaemonLog = nullptr;
            }

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.hStdInput = hStdinRd;
            si.hStdOutput = hStdoutWr;
            si.hStdError = hDaemonLog;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi{};
            std::wstring cmdLine = quoteWindowsArgument(pythonExePath.wstring()) + L" -B -u " +
                                   quoteWindowsArgument(daemonScriptPath.wstring()) + L" --runtime-root " +
                                   quoteWindowsArgument(runtimeRootDir.wstring());
            if (!managedStateRootDir.empty()) {
                cmdLine += L" --state-root " + quoteWindowsArgument(managedStateRootDir.wstring());
            }
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            const BOOL ok = CreateProcessW(
                pythonExePath.c_str(),
                cmdBuf.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi);

            CloseHandle(hStdinRd);
            CloseHandle(hStdoutWr);

            if (!ok) {
                CloseHandle(hStdinWr);
                CloseHandle(hStdoutRd);
                busyAtom.store(false, std::memory_order_release);
                return false;
            }

            hChildProcess = pi.hProcess;
            CloseHandle(pi.hThread);
            hChildThread = nullptr;
            hPipeWriteStdin = hStdinWr;
            hPipeReadStdout = hStdoutRd;

            // Bind the daemon to a kill-on-close Job so a crashed / force-killed host
            // never leaves an orphan python.exe holding GPU memory.
            if (!hKillJob) {
                hKillJob = CreateJobObjectW(nullptr, nullptr);
                if (hKillJob) {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
                    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (!SetInformationJobObject(hKillJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
                        CloseHandle(hKillJob);
                        hKillJob = nullptr;
                    }
                }
            }
            if (hKillJob) {
                AssignProcessToJobObject(hKillJob, hChildProcess);
            }

            // Send CMD_INIT to load HuBERT + RMVPE base models on CUDA (slots stay empty until loaded)
            PipePacketHeader initHdr{{'R', 'V', 'C', 'Q'}, kCmdInit, 0};
            std::vector<uint8_t> initResp;
            if (!writeExactPipe(hPipeWriteStdin, &initHdr, sizeof(initHdr)) ||
                !receiveDaemonResponse(kMsgStatusOk, initResp, progAtom, onProgress)) {
                // Tear down the half-initialized daemon so the next call respawns cleanly
                shutdownDaemonLocked();
                readyAtom.store(false, std::memory_order_release);
                busyAtom.store(false, std::memory_order_release);
                return false;
            }

            // Re-queue any user-loaded slot models so the newly spawned daemon loads them in Step 3
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                for (int s = 0; s < 2; ++s) {
                    if (slots[s].pendingAction == PendingSlotAction::kNone && slots[s].info.isLoaded &&
                        !slots[s].info.filePath.empty()) {
                        slots[s].pendingFilePath = utf8ToWide(slots[s].info.filePath);
                        slots[s].pendingIndexPath = utf8ToWide(slots[s].info.indexPath);
                        slots[s].explicitIndex = true;
                        slots[s].pendingAction = PendingSlotAction::kLoadFile;
                    }
                }
            }
            daemonSpawnAttempted = true;

            readyAtom.store(true, std::memory_order_release);
            busyAtom.store(false, std::memory_order_release);
        }

        // 3. Synchronize any pending slot actions queued by UI / Host
        for (int s = 0; s < 2; ++s) {
            PendingSlotAction action = PendingSlotAction::kNone;
            std::wstring pendingPath;
            std::wstring pendingIndex;
            bool explicitIndex = false;
            uint64_t selectionRevision = 0;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                action = slots[s].pendingAction;
                pendingPath = slots[s].pendingFilePath;
                pendingIndex = slots[s].pendingIndexPath;
                explicitIndex = slots[s].explicitIndex;
                selectionRevision = slots[s].selectionRevision;
            }

            if (action == PendingSlotAction::kNone) continue;

            busyAtom.store(true, std::memory_order_release);
            std::vector<uint8_t> resp;
            bool ok = false;
            if (action == PendingSlotAction::kClear) {
                uint32_t slotIdxU32 = static_cast<uint32_t>(s);
                PipePacketHeader hdr{{'R', 'V', 'C', 'Q'}, kCmdClearSlot, sizeof(slotIdxU32)};
                ok = writeExactPipe(hPipeWriteStdin, &hdr, sizeof(hdr)) &&
                    writeExactPipe(hPipeWriteStdin, &slotIdxU32, sizeof(slotIdxU32)) &&
                    receiveDaemonResponse(kMsgSlotInfo, resp, progAtom, onProgress);
            } else if (action == PendingSlotAction::kLoadFile && !pendingPath.empty()) {
                const std::string utf8Path = wideToUtf8(pendingPath);
                const uint32_t slotIdxU32 = static_cast<uint32_t>(s);
                const uint32_t pathLenU32 = static_cast<uint32_t>(utf8Path.size());
                std::vector<uint8_t> reqPayload(8 + pathLenU32);
                std::memcpy(reqPayload.data(), &slotIdxU32, 4);
                std::memcpy(reqPayload.data() + 4, &pathLenU32, 4);
                std::memcpy(reqPayload.data() + 8, utf8Path.data(), pathLenU32);
                if (explicitIndex) {
                    const std::string index = wideToUtf8(pendingIndex);
                    const uint32_t indexSize = static_cast<uint32_t>(index.size());
                    reqPayload.resize(12 + pathLenU32 + indexSize);
                    std::memcpy(reqPayload.data() + 8 + pathLenU32, &indexSize, 4);
                    std::memcpy(reqPayload.data() + 12 + pathLenU32, index.data(), indexSize);
                }

                PipePacketHeader hdr{{'R', 'V', 'C', 'Q'}, kCmdLoadSlot, static_cast<uint32_t>(reqPayload.size())};
                ok = writeExactPipe(hPipeWriteStdin, &hdr, sizeof(hdr)) &&
                    writeExactPipe(hPipeWriteStdin, reqPayload.data(), reqPayload.size()) &&
                    receiveDaemonResponse(kMsgSlotInfo, resp, progAtom, onProgress);
            }
            busyAtom.store(false, std::memory_order_release);
            if (ok && resp.size() >= offsetof(DaemonSlotInfoWire, indexPath)) {
                DaemonSlotInfoWire wire{};
                std::memcpy(&wire, resp.data(), std::min(resp.size(), sizeof(wire)));
                if (wire.slotIdx == static_cast<uint32_t>(s) &&
                    ((action == PendingSlotAction::kClear && !wire.isLoaded) ||
                     (action == PendingSlotAction::kLoadFile && wire.isLoaded))) {
                    applyWireSlotInfo(wire, selectionRevision, pendingIndex);
                    continue;
                }
            }
            const bool connectionFailed = !ioFailureText.empty() || ok || action == PendingSlotAction::kClear;
            if (!connectionFailed) {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (slots[s].selectionRevision == selectionRevision) {
                    slots[s].pendingAction = PendingSlotAction::kNone;
                    slots[s].pendingFilePath = utf8ToWide(slots[s].info.filePath);
                    slots[s].pendingIndexPath = utf8ToWide(slots[s].info.indexPath);
                    slots[s].explicitIndex = true;
                }
            }
            // Protocol/I/O failures invalidate the connection; a model error leaves the old slot intact.
            if (connectionFailed) {
                setStatus(progAtom, 1.0f, "ERR: " + (ioFailureText.empty() ? "INVALID RVC SLOT RESPONSE" : ioFailureText));
                shutdownDaemonLocked();
                readyAtom.store(false, std::memory_order_release);
            }
            return false;
        }

        std::lock_guard<std::mutex> stateLock(stateMutex);
        return readyAtom.load(std::memory_order_acquire) &&
               slots[0].pendingAction == PendingSlotAction::kNone &&
               slots[1].pendingAction == PendingSlotAction::kNone;
    }
};

RvcModelEngine::RvcModelEngine() : impl_(std::make_unique<Impl>()) {}

RvcModelEngine::~RvcModelEngine() = default;

std::string RvcModelEngine::getLoadingStatusText() const {
    std::lock_guard<std::mutex> lock(impl_->statusMutex);
    return impl_->loadingStatusText;
}

std::wstring RvcModelEngine::getRuntimeRootPath() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->runtimeRootDir.wstring();
}

bool RvcModelEngine::initialize(const std::wstring& vstModuleDir) {
    fs::path modDir = vstModuleDir.empty() ? fs::current_path() : fs::path(vstModuleDir);
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        impl_->pythonExePath.clear();
        impl_->daemonScriptPath.clear();
        impl_->runtimeRootDir.clear();
        impl_->managedStateRootDir.clear();
        std::vector<fs::path> locations{modDir.parent_path() / L"Resources", modDir};
        auto ancestor = modDir.parent_path();
        for (int i = 0; i < 4 && !ancestor.empty(); ++i, ancestor = ancestor.parent_path()) {
            locations.push_back(ancestor);
        }
        for (const auto& location : locations) {
            const auto candidate = location / L"tools" / L"rvc_daemon_worker.py";
            if (fs::is_regular_file(candidate)) { impl_->daemonScriptPath = candidate; break; }
        }

        for (const auto& location : locations) {
            const auto configPath = location / L"state-root.txt";
            if (!fs::is_regular_file(configPath)) continue;
            std::ifstream config(configPath, std::ios::binary);
            std::string line;
            std::getline(config, line);
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            const auto first = line.find_first_not_of(" \t\r\n");
            const auto last = line.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) {
                const auto configured = utf8ToWide(line.substr(first, last - first + 1));
                impl_->managedStateRootDir = resolveManagedStateRoot(configured,
                    configured.rfind(L"%LOCALAPPDATA%", 0) == 0 ? currentUserLocalAppData() : fs::path{});
            }
            if (impl_->managedStateRootDir.empty()) {
                ortInitialized_.store(false, std::memory_order_release);
                impl_->setStatus(loadingProgress_, 1.0f, "ERR: INVALID Resources/state-root.txt; EXPECTED AN ABSOLUTE STATE FOLDER");
                return false;
            }
            break;
        }

        wchar_t configuredRoot[32768]{};
        const DWORD envLength = GetEnvironmentVariableW(L"RVC_RUNTIME_ROOT", configuredRoot, 32768);
        if (envLength > 0 && envLength < 32768) impl_->runtimeRootDir = configuredRoot;
        if (impl_->runtimeRootDir.empty()) {
            for (const auto& location : locations) {
                const auto configPath = location / L"runtime-root.txt";
                if (!fs::is_regular_file(configPath)) continue;
                std::ifstream config(configPath, std::ios::binary);
                std::string line;
                std::getline(config, line);
                if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
                const auto first = line.find_first_not_of(" \t\r\n");
                const auto last = line.find_last_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    fs::path root(utf8ToWide(line.substr(first, last - first + 1)));
                    impl_->runtimeRootDir = root.is_absolute() ? root : configPath.parent_path() / root;
                }
                // An explicit but invalid configuration must not silently select another runtime.
                if (impl_->runtimeRootDir.empty()) impl_->runtimeRootDir = configPath / L"INVALID_RUNTIME_ROOT";
                break;
            }
        }
        if (impl_->runtimeRootDir.empty()) {
            for (const auto& location : locations) {
                const auto bundled = location / L"runtime";
                if (fs::is_regular_file(bundled / L"runtime" / L"python.exe") ||
                    fs::is_regular_file(bundled / L"python.exe")) {
                    impl_->runtimeRootDir = bundled; break;
                }
            }
        }
        if (impl_->runtimeRootDir.empty()) impl_->runtimeRootDir = L"D:\\RVC20260718Nvidia";
        const std::vector<fs::path> pythonCandidates{
            impl_->runtimeRootDir / L"runtime" / L"python.exe",
            impl_->runtimeRootDir / L"python.exe",
            impl_->runtimeRootDir / L".venv" / L"Scripts" / L"python.exe",
            impl_->runtimeRootDir / L"Scripts" / L"python.exe"};
        for (const auto& candidate : pythonCandidates) {
            if (fs::is_regular_file(candidate)) { impl_->pythonExePath = candidate; break; }
        }
    }

    const bool available = !impl_->pythonExePath.empty() && !impl_->daemonScriptPath.empty();
    ortInitialized_.store(available, std::memory_order_release);
    // Note: Do NOT auto-load any default models on initialization.
    // Both Slot A and Slot B start in UNLOADED state until imported by the user.
    if (!available) impl_->setStatus(loadingProgress_, 1.0f, "ERR: RVC RUNTIME NOT FOUND; SET RVC_RUNTIME_ROOT OR Resources/runtime-root.txt");
    return available;
}

void RvcModelEngine::clearSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex > 1) return;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        auto& slot = impl_->slots[slotIndex];
        const int32_t savedOctave = slot.info.targetOctaveSemitones;
        slot.info = RvcModelSlotInfo{};
        slot.info.targetOctaveSemitones = savedOctave;
        slot.pendingFilePath.clear();
        slot.pendingIndexPath.clear();
        slot.explicitIndex = true;
        ++slot.selectionRevision;
        slot.pendingAction = PendingSlotAction::kClear;
    }
    impl_->setStatus(loadingProgress_, 1.0f, (slotIndex == 0) ? "SLOT A CLEARED" : "SLOT B CLEARED");
    modelRevision_.fetch_add(1, std::memory_order_release);
}

void RvcModelEngine::setSlotOctaveShift(int slotIndex, int32_t semitones) {
    if (slotIndex < 0 || slotIndex > 1) return;
    const int32_t clamped = std::clamp(semitones, -24, 24);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        if (impl_->slots[slotIndex].info.targetOctaveSemitones != clamped) {
            impl_->slots[slotIndex].info.targetOctaveSemitones = clamped;
            changed = true;
        }
    }
    if (changed) {
        modelRevision_.fetch_add(1, std::memory_order_release);
    }
}

int32_t RvcModelEngine::getSlotOctaveShift(int slotIndex) const {
    if (slotIndex < 0 || slotIndex > 1) return 0;
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->slots[slotIndex].info.targetOctaveSemitones;
}

bool RvcModelEngine::loadModelIntoSlot(int slotIndex, const std::wstring& filePath) {
    if (slotIndex < 0 || slotIndex > 1 || filePath.empty()) return false;
    fs::path selected(filePath);
    std::wstring extension = selected.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension == L".pth") {
        std::error_code ec;
        std::vector<fs::path> candidates;
        std::vector<fs::path> matching;
        std::wstring selectedStem = selected.stem().wstring();
        std::transform(selectedStem.begin(), selectedStem.end(), selectedStem.begin(), ::towlower);
        for (const auto& entry : fs::directory_iterator(selected.parent_path().empty() ? fs::path(L".") : selected.parent_path(), ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext != L".index") continue;
            candidates.push_back(entry.path());
            std::wstring stem = entry.path().stem().wstring();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
            if (stem.find(selectedStem) != std::wstring::npos) matching.push_back(entry.path());
        }
        if (!matching.empty()) candidates = matching;
        if (candidates.size() > 1) {
            std::vector<fs::path> added;
            for (const auto& candidate : candidates) {
                std::wstring name = candidate.filename().wstring();
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                if (name.rfind(L"added", 0) == 0) added.push_back(candidate);
            }
            if (!added.empty()) candidates = added;
        }
        if (candidates.size() > 1) {
            impl_->setStatus(loadingProgress_, 1.0f, "ERR: MULTIPLE INDEX FILES; SELECT AN EXPLICIT MODEL/INDEX PAIR");
            return false;
        }
        return queueModelLoad(slotIndex, filePath, candidates.empty() ? L"" : candidates.front().wstring(), true);
    }
    if (extension != L".index") {
        impl_->setStatus(loadingProgress_, 1.0f, "ERR: NATIVE RVC REQUIRES .pth OR .index FILES");
        return false;
    }
    std::error_code ec;
    std::vector<fs::path> checkpoints;
    std::vector<fs::path> matching;
    std::wstring selectedStem = selected.stem().wstring();
    std::transform(selectedStem.begin(), selectedStem.end(), selectedStem.begin(), ::towlower);
    for (const auto& entry : fs::directory_iterator(selected.parent_path().empty() ? fs::path(L".") : selected.parent_path(), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".pth") continue;
        checkpoints.push_back(entry.path());
        std::wstring stem = entry.path().stem().wstring();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
        if (selectedStem.find(stem) != std::wstring::npos) matching.push_back(entry.path());
    }
    const fs::path checkpoint = checkpoints.size() == 1 ? checkpoints.front() :
                                matching.size() == 1 ? matching.front() : fs::path{};
    if (checkpoint.empty()) {
        const auto current = getSlotSelection(slotIndex);
        const fs::path currentPath = utf8ToWide(current.filePath);
        std::error_code pathError;
        if (!currentPath.empty() && fs::equivalent(currentPath.parent_path(), selected.parent_path(), pathError)) {
            return queueModelLoad(slotIndex, currentPath.wstring(), filePath, true);
        }
        impl_->setStatus(loadingProgress_, 1.0f, "ERR: INDEX NEEDS AN UNAMBIGUOUS .pth CHECKPOINT; LOAD A CHECKPOINT FIRST");
        return false;
    }
    return queueModelLoad(slotIndex, checkpoint.wstring(), filePath, true);
}

bool RvcModelEngine::loadModelIntoSlot(int slotIndex, const std::wstring& filePath, const std::wstring& indexPath) {
    return queueModelLoad(slotIndex, filePath, indexPath, true);
}

bool RvcModelEngine::queueModelLoad(int slotIndex, const std::wstring& filePath,
                                  const std::wstring& indexPath, bool explicitIndex) {
    if (slotIndex < 0 || slotIndex > 1 || filePath.empty()) return false;
    const auto validPath = [](const std::wstring& path) {
        return std::none_of(path.begin(), path.end(), [](wchar_t ch) { return ch < 32 || ch == L'"'; });
    };
    std::error_code ec;
    fs::path checkpoint = fs::absolute(fs::path(filePath), ec).lexically_normal();
    if (ec || !validPath(filePath) || !validPath(indexPath) || !fs::is_regular_file(checkpoint, ec)) return false;
    fs::path index;
    if (!indexPath.empty()) {
        index = fs::absolute(fs::path(indexPath), ec).lexically_normal();
        if (ec || !fs::is_regular_file(index, ec)) return false;
    }
    std::wstring extension = checkpoint.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".pth") return false;
    if (!index.empty()) {
        std::wstring indexExtension = index.extension().wstring();
        std::transform(indexExtension.begin(), indexExtension.end(), indexExtension.begin(), ::towlower);
        if (indexExtension != L".index") return false;
    }
    if (wideToUtf8(checkpoint.wstring()).size() >= 512 || wideToUtf8(index.wstring()).size() >= 512) {
        impl_->setStatus(loadingProgress_, 1.0f, "ERR: RVC MODEL/INDEX PATH EXCEEDS 511 UTF-8 BYTES");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        auto& slot = impl_->slots[slotIndex];
        slot.pendingFilePath = checkpoint.wstring();
        slot.pendingIndexPath = index.wstring();
        slot.explicitIndex = explicitIndex;
        slot.pendingAction = PendingSlotAction::kLoadFile;
        ++slot.selectionRevision;
    }
    impl_->setStatus(loadingProgress_, 0.15f, "QUEUED RVC MODEL: " + wideToUtf8(checkpoint.stem().wstring()));
    modelRevision_.fetch_add(1, std::memory_order_release);
    return true;
}
RvcModelSlotInfo RvcModelEngine::getSlotInfo(int slotIndex) const {
    if (slotIndex < 0 || slotIndex > 1) return {};
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->slots[slotIndex].info;
}

RvcModelSlotInfo RvcModelEngine::getSlotSelection(int slotIndex) const {
    if (slotIndex < 0 || slotIndex > 1) return {};
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    const auto& slot = impl_->slots[slotIndex];
    auto info = slot.info;
    if (slot.pendingAction == PendingSlotAction::kLoadFile) {
        info.isLoaded = false;
        info.filePath = wideToUtf8(slot.pendingFilePath);
        info.indexPath = wideToUtf8(slot.pendingIndexPath);
        info.modelName = wideToUtf8(fs::path(slot.pendingFilePath).stem().wstring());
    }
    return info;
}

void RvcModelEngine::cancelPendingWork() noexcept {
    impl_->cancelWork();
    daemonReady_.store(false, std::memory_order_release);
}

void RvcModelEngine::resumePendingWork() noexcept {
    std::lock_guard<std::mutex> lock(impl_->cancelMutex);
    impl_->cancellationLatched = false;
}

bool RvcModelEngine::syncDaemonSlotsIfNeeded() {
    bool anyDirty = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        anyDirty = (impl_->slots[0].pendingAction != PendingSlotAction::kNone) ||
                   (impl_->slots[1].pendingAction != PendingSlotAction::kNone);
    }
    if (!anyDirty) {
        return true;
    }
    const auto generation = impl_->cancelGeneration.load(std::memory_order_acquire);
    return impl_->ensureDaemonStartedAndSynced(generation, daemonReady_, daemonBusy_, loadingProgress_);
}

bool RvcModelEngine::synthesizeRvcChunk(
    const float* input16k,
    size_t numSamples16k,
    const RvcChunkSynthRequest& req,
    RvcChunkSynthResult& outResult,
    const std::function<void(float, const std::string&)>& onProgress) {

    if (!input16k || numSamples16k == 0 || numSamples16k > 16000 * 60) {
        return false;
    }

    const auto generation = impl_->cancelGeneration.load(std::memory_order_acquire);
    if (!impl_->ensureDaemonStartedAndSynced(generation, daemonReady_, daemonBusy_, loadingProgress_, onProgress)) {
        return false;
    }

    std::lock_guard<std::mutex> ipcLock(impl_->ipcMutex);
    if (!impl_->beginOperation(generation)) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        if ((!impl_->slots[0].info.isLoaded && !impl_->slots[1].info.isLoaded) ||
            impl_->slots[0].pendingAction != PendingSlotAction::kNone ||
            impl_->slots[1].pendingAction != PendingSlotAction::kNone) {
            impl_->setStatus(loadingProgress_, 1.0f, "ERR: RVC MODEL IS NOT LOADED");
            return false;
        }
    }
    daemonBusy_.store(true, std::memory_order_release);

    const uint32_t numAutoFrames = static_cast<uint32_t>(req.automationCurves4Col.size() / 4);
    const size_t validAutoFloats = static_cast<size_t>(numAutoFrames) * 4;

    SynthRequestWireHeader wireReq{};
    wireReq.abMorph = std::clamp(req.abMorph, 0.0f, 1.0f);
    wireReq.injectDepth = std::clamp(req.injectDepth, 0.0f, 1.0f);
    wireReq.pitchShiftSt = std::clamp(req.pitchShiftSt + req.slotAPitchSt, -24.0f, 24.0f);
    wireReq.f0FilterRadius = std::clamp(req.pitchShiftSt + req.slotBPitchSt, -24.0f, 24.0f);
    wireReq.indexRate = std::clamp(req.indexRate, 0.0f, 1.0f);
    wireReq.protect = std::clamp(req.protect, 0.0f, 0.50f);
    wireReq.slotAIndexRate = std::clamp(req.slotAIndexRate, 0.0f, 1.0f);
    wireReq.slotBIndexRate = std::clamp(req.slotBIndexRate, 0.0f, 1.0f);
    wireReq.rmsMixRate = std::clamp(req.rmsMixRate, 0.0f, 1.0f);
    wireReq.targetSr = std::clamp<uint32_t>(req.targetSampleRate, 16000, 96000);
    wireReq.numSamples16k = static_cast<uint32_t>(numSamples16k);
    wireReq.numAutoFrames = numAutoFrames;

    const size_t audioBytes = numSamples16k * sizeof(float);
    const size_t autoBytes = validAutoFloats * sizeof(float);
    const uint32_t totalPayloadLen = static_cast<uint32_t>(sizeof(wireReq) + audioBytes + autoBytes);
    PipePacketHeader hdr{{'R', 'V', 'C', 'Q'}, kCmdSynthChunk, totalPayloadLen};

    std::vector<uint8_t> respPayload;
    bool ok = impl_->writeExactPipe(impl_->hPipeWriteStdin, &hdr, sizeof(hdr)) &&
              impl_->writeExactPipe(impl_->hPipeWriteStdin, &wireReq, sizeof(wireReq)) &&
              impl_->writeExactPipe(impl_->hPipeWriteStdin, input16k, audioBytes);
    if (ok && autoBytes > 0) {
        ok = impl_->writeExactPipe(impl_->hPipeWriteStdin, req.automationCurves4Col.data(), autoBytes);
    }
    if (ok) {
        ok = impl_->receiveDaemonResponse(kMsgSynthResult, respPayload, loadingProgress_, onProgress);
    }

    daemonBusy_.store(false, std::memory_order_release);

    if (!ok || respPayload.size() < sizeof(SynthResultWireHeader)) {
        if (!impl_->ioFailureText.empty() || ok) {
            impl_->setStatus(loadingProgress_, 1.0f, "ERR: " + (impl_->ioFailureText.empty() ?
                "INVALID RVC SYNTHESIS RESPONSE" : impl_->ioFailureText));
            impl_->shutdownDaemonLocked();
            daemonReady_.store(false, std::memory_order_release);
        }
        return false;
    }

    SynthResultWireHeader resHdr{};
    std::memcpy(&resHdr, respPayload.data(), sizeof(resHdr));

    const size_t availAudioBytes = respPayload.size() - sizeof(SynthResultWireHeader);
    const size_t totalFloatsAvail = availAudioBytes / sizeof(float);
    const size_t validSamples = resHdr.numOutSamples;
    if (validSamples == 0 || validSamples != totalFloatsAvail / 2 ||
        availAudioBytes != validSamples * 2 * sizeof(float)) {
        impl_->setStatus(loadingProgress_, 1.0f, "ERR: INVALID RVC AUDIO PAYLOAD LENGTH");
        impl_->shutdownDaemonLocked();
        daemonReady_.store(false, std::memory_order_release);
        return false;
    }
    // Python and C++ round exact halves differently; at most one output sample can differ.
    const size_t expectedSamples = static_cast<size_t>(std::llround(
        static_cast<double>(numSamples16k) * wireReq.targetSr / 16000.0));
    if (validSamples + 1 < expectedSamples || validSamples > expectedSamples + 1) {
        impl_->setStatus(loadingProgress_, 1.0f, "ERR: RVC AUDIO LENGTH DOES NOT MATCH REQUEST");
        impl_->shutdownDaemonLocked();
        daemonReady_.store(false, std::memory_order_release);
        return false;
    }

    outResult.detectedPitchHz = std::isfinite(resHdr.detectedF0Hz) ? resHdr.detectedF0Hz : 0.0f;
    outResult.inferenceMs = std::isfinite(resHdr.elapsedMs) ? resHdr.elapsedMs : 0.0f;
    lastInferenceMs_.store(outResult.inferenceMs, std::memory_order_relaxed);

    std::memcpy(outResult.dryEnvDb.data(), resHdr.dryEnv64, sizeof(resHdr.dryEnv64));
    std::memcpy(outResult.slotAEnvDb.data(), resHdr.slotAEnv64, sizeof(resHdr.slotAEnv64));
    std::memcpy(outResult.slotBEnvDb.data(), resHdr.slotBEnv64, sizeof(resHdr.slotBEnv64));
    std::memcpy(outResult.injectedEnvDb.data(), resHdr.outEnv64, sizeof(resHdr.outEnv64));

    outResult.slotAAudioSamples.resize(validSamples);
    outResult.slotBAudioSamples.resize(validSamples);
    const uint8_t* audioBase = respPayload.data() + sizeof(SynthResultWireHeader);
    std::memcpy(
        outResult.slotAAudioSamples.data(),
        audioBase,
        validSamples * sizeof(float));
    std::memcpy(
        outResult.slotBAudioSamples.data(),
        audioBase + validSamples * sizeof(float),
        validSamples * sizeof(float));

    for (float& s : outResult.slotAAudioSamples) {
        if (!std::isfinite(s)) s = 0.0f;
        else s = std::clamp(s, -1.5f, 1.5f);
    }
    for (float& s : outResult.slotBAudioSamples) {
        if (!std::isfinite(s)) s = 0.0f;
        else s = std::clamp(s, -1.5f, 1.5f);
    }

    return true;
}

} // namespace rvc_vst
