#include "../src/inference/RvcModelEngine.h"
#include <windows.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using rvc_vst::RvcModelEngine;
namespace rvc_vst { bool verifyManagedStateRootResolutionForTest(const std::wstring& fixtureRoot); }

static void check(bool condition, const char* description) {
    if (!condition) { std::cerr << "FAIL: " << description << std::endl; std::exit(1); }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    const fs::path base = fs::absolute(argv[1]);
    const std::wstring mode = argv[2];
    if (mode == L"state-paths") {
        check(rvc_vst::verifyManagedStateRootResolutionForTest(base.wstring()), "managed state token resolves only the exact current-user LocalAppData prefix");
        RvcModelEngine engine;
        check(engine.initialize(base.wstring()), "production initialize accepts current-user state token without starting worker or writing state");
        std::cout << "PASS managed state path resolution (read-only known folder lookup)" << std::endl;
        return 0;
    }
    const auto models = base / L"fixtures";
    fs::create_directories(models);
    for (const auto* name : {L"good.pth", L"bad.pth", L"slow_good.pth", L"last_good.pth", L"z_selected.index", L"added_first.index"}) {
        std::ofstream file(models / name); file << "protocol fixture only";
    }
    const auto good = (models / L"good.pth").wstring();
    const auto index = (models / L"z_selected.index").wstring();
    SetEnvironmentVariableW(L"ENGINE_FIX_HANG", mode == L"transaction" ? nullptr : mode.c_str());
    SetEnvironmentVariableW(L"RVC_IPC_TIMEOUT_MS", mode == L"timeout" ? L"350" : L"10000");
    RvcModelEngine engine;
    check(engine.initialize(base.wstring()), "locate external runtime and fixture worker");
    check(!engine.getSlotSelection(0).isLoaded && engine.getSlotSelection(0).filePath.empty() &&
          !engine.getSlotSelection(1).isLoaded && engine.getSlotSelection(1).filePath.empty(),
          "default slots do not select a singer model");
    check(engine.loadModelIntoSlot(0, good, index), "queue initial selection");
    check(!engine.getSlotInfo(0).isLoaded, "queued models are not reported as loaded");
    check(!engine.getSlotSelection(0).filePath.empty() && !engine.getSlotSelection(0).indexPath.empty(), "pending pair is persistable");

    if (mode == L"transaction") {
        check(engine.syncDaemonSlotsIfNeeded(), "successful model transaction");
        const auto confirmed = engine.getSlotInfo(0);
        check(confirmed.isLoaded && confirmed.modelName == "good", "confirmed metadata");
        check(fs::u8path(confirmed.indexPath) == fs::path(index), "explicit index preserved in ACK");
        check(engine.loadModelIntoSlot(0, (models / L"bad.pth").wstring(), index), "queue invalid checkpoint");
        check(engine.getSlotInfo(0).modelName == "good", "pending failure does not replace metadata");
        check(!engine.syncDaemonSlotsIfNeeded(), "model error propagates as false");
        check(engine.getSlotInfo(0).modelName == "good", "model error keeps confirmed metadata");
        check(engine.getLoadingStatusText().find("invalid checkpoint fixture") != std::string::npos, "load error stays visible");
        rvc_vst::RvcChunkSynthRequest request;
        rvc_vst::RvcChunkSynthResult result;
        const float samples[100]{};
        check(engine.synthesizeRvcChunk(samples, 100, request, result), "synthesis after failed switch");
        check(result.slotAAudioSamples.front() == 0.125f, "synthesis and confirmed model agree");
        check(engine.loadModelIntoSlot(0, index), "selected index reuses confirmed checkpoint");
        check(engine.syncDaemonSlotsIfNeeded(), "selected index synchronized");
        check(fs::u8path(engine.getSlotInfo(0).indexPath) == fs::path(index), "index is not silently replaced");

        check(engine.loadModelIntoSlot(0, (models / L"slow_good.pth").wstring(), L""), "queue slow load");
        bool firstSync = true;
        std::thread worker([&] { firstSync = engine.syncDaemonSlotsIfNeeded(); });
        for (int i = 0; i < 200 && !engine.isDaemonBusy(); ++i) Sleep(5);
        Sleep(80);
        check(engine.loadModelIntoSlot(0, (models / L"last_good.pth").wstring(), L""), "new selection while load in flight");
        worker.join();
        check(!firstSync, "superseded selection cannot permit synthesis");
        check(engine.getSlotInfo(0).modelName == "good", "stale ACK cannot replace current metadata");
        check(engine.syncDaemonSlotsIfNeeded(), "newest selection completes");
        check(engine.getSlotInfo(0).modelName == "last_good", "newest model committed");
        check(engine.getSlotInfo(0).indexPath.empty(), "explicit empty index disables retrieval");
        engine.cancelPendingWork();
        engine.resumePendingWork();
        SetEnvironmentVariableW(L"ENGINE_FIX_REJECT_LOAD", L"1");
        check(!engine.synthesizeRvcChunk(samples, 100, request, result), "failed recovery after daemon restart propagates");
        check(!engine.getSlotInfo(0).isLoaded, "new daemon cannot inherit old loaded metadata");
        check(!engine.synthesizeRvcChunk(samples, 100, request, result), "no model cannot silently render dry as RVC");
        SetEnvironmentVariableW(L"ENGINE_FIX_REJECT_LOAD", nullptr);
        engine.cancelPendingWork();
        engine.resumePendingWork();
        check(engine.loadModelIntoSlot(0, good, index), "requeue valid pair after failed recovery");
        check(engine.syncDaemonSlotsIfNeeded(), "recover valid pair in a new daemon");
        engine.clearSlot(0);
        check(!engine.getSlotInfo(0).isLoaded && engine.syncDaemonSlotsIfNeeded(), "clear ACK and metadata agree");
    } else if (mode == L"reply-length") {
        for (const auto* replyMode : {L"", L"one-short", L"one-long", L"short", L"long", L"empty"}) {
            SetEnvironmentVariableW(L"ENGINE_FIX_REPLY", replyMode);
            RvcModelEngine responseEngine;
            check(responseEngine.initialize(base.wstring()), "initialize response-length fixture");
            check(responseEngine.loadModelIntoSlot(0, good, index) && responseEngine.syncDaemonSlotsIfNeeded(),
                  "load response-length fixture model");
            const bool accepted = std::wstring(replyMode).empty() || std::wstring(replyMode) == L"one-short" ||
                                  std::wstring(replyMode) == L"one-long";
            // 46 and 93 are 128/256 host frames at 44.1 kHz resampled to 16 kHz.
            const std::vector<size_t> lengths = std::wstring(replyMode).empty()
                ? std::vector<size_t>{46, 93, 4001, 96017, 80} : std::vector<size_t>{46};
            for (const size_t length : lengths) {
                const std::vector<float> samples(length, 0.1f);
                rvc_vst::RvcChunkSynthRequest request;
                request.targetSampleRate = 44100;
                rvc_vst::RvcChunkSynthResult result;
                check(responseEngine.synthesizeRvcChunk(samples.data(), samples.size(), request, result) == accepted,
                      "response duration must match request within one output sample");
                if (accepted) {
                    const auto expected = static_cast<size_t>(std::llround(length * 44100.0 / 16000.0));
                    check(result.slotAAudioSamples.size() + 1 >= expected && result.slotAAudioSamples.size() <= expected + 1 &&
                          result.slotBAudioSamples.size() == result.slotAAudioSamples.size(), "44.1 kHz response rounding");
                } else {
                    check(responseEngine.getLoadingStatusText().find("AUDIO") != std::string::npos,
                          "invalid audio response reports a clear error");
                    check(!responseEngine.isNativeDaemonReady() && !responseEngine.isDaemonBusy(),
                          "invalid response disconnects daemon without publishing audio");
                }
            }
        }
        SetEnvironmentVariableW(L"ENGINE_FIX_REPLY", nullptr);
    } else {
        bool completed = true;
        const auto start = std::chrono::steady_clock::now();
        std::thread worker([&] {
            completed = engine.syncDaemonSlotsIfNeeded();
            if (completed && (mode == L"synth-read" || mode == L"synth-write")) {
                std::vector<float> samples(960000, 0.1f);
                rvc_vst::RvcChunkSynthRequest request;
                rvc_vst::RvcChunkSynthResult result;
                completed = engine.synthesizeRvcChunk(samples.data(), samples.size(), request, result);
            }
        });
        if (mode != L"timeout") {
            Sleep(400);
            engine.cancelPendingWork();
        }
        worker.join();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(!completed && seconds < 2.0, "hung IPC exits within a bounded time");
        check(!engine.isNativeDaemonReady() && !engine.isDaemonBusy(), "failed connection resets ready/busy");
        std::cout << "bounded_seconds=" << seconds << std::endl;
        SetEnvironmentVariableW(L"ENGINE_FIX_HANG", nullptr);
        SetEnvironmentVariableW(L"RVC_IPC_TIMEOUT_MS", L"10000");
        if (mode != L"timeout") {
            check(engine.loadModelIntoSlot(0, good, index), "queue during stop latch");
            check(!engine.syncDaemonSlotsIfNeeded(), "stopped worker cannot restart the daemon");
            engine.resumePendingWork();
        }
        check(engine.loadModelIntoSlot(0, good, index), "requeue after cancellation/timeout");
        check(engine.syncDaemonSlotsIfNeeded(), "restart after cancellation/timeout");
        check(engine.getSlotInfo(0).isLoaded, "restarted daemon commits model");
    }
    std::wcout << L"PASS engine " << mode << std::endl;
}
