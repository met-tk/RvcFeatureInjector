#include "../src/dsp/RvcFeatureInjector.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace rvc_vst;
namespace {
std::mutex observedMutex;
std::vector<RvcChunkSynthRequest> observedRequests;
std::vector<std::vector<float>> observedInputs;
float toneHz = 0.0f;
std::atomic<bool> holdSynthesis{false};
std::atomic<bool> synthesisEntered{false};
constexpr double sampleRate = 48000.0;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void clearObserved() { std::lock_guard<std::mutex> lock(observedMutex); observedRequests.clear(); observedInputs.clear(); }
void feed(RvcFeatureInjector& dsp, int64_t begin, int count, float value = 0.2f, int block = 512) {
    std::vector<float> input(block, value), output(block);
    for (int n = 0; n < count; n += block)
        dsp.processBlock(input.data(), nullptr, output.data(), nullptr, std::min(block, count-n), 1, begin+n, true);
}
void generate(RvcFeatureInjector& dsp) { dsp.triggerGenerate(); require(dsp.waitForSynthesisIdle(4000), "synthesis did not finish"); }
double dominantHz(const std::vector<float>& audio, double nearHz) {
    double best = -1, frequency = 0;
    for (double hz = nearHz*0.7; hz <= nearHz*1.3; hz += 0.5) {
        double re=0, im=0;
        for (size_t i=48000; i<86400; i+=2) {
            double phase=2*3.141592653589793*hz*i/sampleRate;
            re+=audio[i]*std::cos(phase); im+=audio[i]*std::sin(phase);
        }
        if (re*re+im*im > best) { best=re*re+im*im; frequency=hz; }
    }
    return frequency;
}
std::vector<float> render(RvcFeatureInjector& dsp, float pitch, int block=512) {
    auto params=dsp.getParameters(); params.pitchShiftSemitones=pitch;
    params.rmsMixRate=0; params.dryWetMix=1; params.injectDepth=1;
    dsp.updateParameters(params); dsp.reset();
    std::vector<float> result(144000), input(block,0);
    for (int n=0;n<144000;n+=block)
        dsp.processBlock(input.data(),nullptr,result.data()+n,nullptr,std::min(block,144000-n),1,n,true);
    return result;
}
}
// Deterministic neural substitute: all production capture, automation, cache and realtime DSP
// code is compiled into this test. GPU/model variability cannot hide a timeline regression.
namespace rvc_vst {
struct RvcCaptureTestPeer {
    static void holdControl(RvcFeatureInjector& dsp) { dsp.lockCaptureControl(); }
    static void releaseControl(RvcFeatureInjector& dsp) { dsp.captureControlBusy_.clear(std::memory_order_release); }
    static bool captured(RvcFeatureInjector& dsp, int64_t projectSample, float expected) {
        int64_t start,end; dsp.getCaptureRange(start,end);
        if (projectSample<start || projectSample>=end) return false;
        const auto generation=dsp.captureGeneration_.load(std::memory_order_acquire);
        const auto index=static_cast<size_t>(projectSample-start);
        return dsp.hasCapturedSample(index,generation,start) && dsp.readCapturedSample(index,generation,start)==expected;
    }
    static bool capturedIndex(RvcFeatureInjector& dsp, int64_t projectSample, float expected) {
        const int64_t projectFrame=dsp.automationFrameAt(projectSample);
        const auto& frame=dsp.automationTimeline_[static_cast<size_t>(projectFrame%RvcFeatureInjector::kAutomationCacheFrames)];
        return frame.projectFrame.load()==projectFrame && frame.generation.load()==dsp.captureGeneration_.load() &&
            frame.slotAIndexRate.load()==expected;
    }
};
struct RvcModelEngine::Impl {};
RvcModelEngine::RvcModelEngine()=default;
RvcModelEngine::~RvcModelEngine()=default;
void RvcModelEngine::cancelPendingWork() noexcept {}
void RvcModelEngine::resumePendingWork() noexcept {}
bool RvcModelEngine::syncDaemonSlotsIfNeeded() { return true; }
RvcModelSlotInfo RvcModelEngine::getSlotInfo(int) const { RvcModelSlotInfo info; info.isLoaded=true; return info; }
std::string RvcModelEngine::getLoadingStatusText() const { return {}; }
bool RvcModelEngine::synthesizeRvcChunk(const float* input, size_t count, const RvcChunkSynthRequest& request,
    RvcChunkSynthResult& result, const std::function<void(float,const std::string&)>&) {
    { std::lock_guard<std::mutex> lock(observedMutex); observedRequests.push_back(request); observedInputs.emplace_back(input,input+count); }
    synthesisEntered.store(true, std::memory_order_release);
    while (holdSynthesis.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const size_t n=static_cast<size_t>(std::llround(count*request.targetSampleRate/16000.0));
    result.slotAAudioSamples.resize(n,0.3f);
    if (toneHz > 0) {
        for(size_t i=0;i<n;++i)
            result.slotAAudioSamples[i]=(i>=33600 && i<105600)
                ? 0.2f*std::cos(2*3.141592653589793*toneHz*(i-33600)/sampleRate) : 0.0f;
    }
    result.slotBAudioSamples=result.slotAAudioSamples;
    return true;
}
}
int main() {
    try {
        for (int rate : {44100,48000,96000}) for (int block : {128,512,1024}) {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine); dsp.prepare(rate,block);
            const int64_t start=rate*20+37;
            const int firstCount=rate/4;
            const int appendCount=rate/4;
            auto sampleAt=[](int64_t sample) { return 0.125f+static_cast<float>(sample%8192)*0.00001f; };
            std::vector<float> input(block),output(block);
            auto process=[&](int64_t pos,int count,bool playing=true) {
                for(int i=0;i<count;++i) input[i]=sampleAt(pos+i);
                dsp.processBlock(input.data(),nullptr,output.data(),nullptr,count,1,pos,playing);
            };
            dsp.setTimelineState(TimelineState::kCapture);
            for(int n=0;n<firstCount;n+=block) process(start+n,std::min(block,firstCount-n));
            dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
            // Hold the actual UI control primitive across the append's first audio callback.
            std::atomic<bool> held{false},release{false};
            std::thread controlThread([&] {
                RvcCaptureTestPeer::holdControl(dsp); held.store(true,std::memory_order_release);
                while(!release.load(std::memory_order_acquire)) std::this_thread::yield();
                RvcCaptureTestPeer::releaseControl(dsp);
            });
            while(!held.load(std::memory_order_acquire)) std::this_thread::yield();
            process(start+firstCount,block);
            release.store(true,std::memory_order_release); controlThread.join();
            for(int n=block;n<appendCount;n+=block) process(start+firstCount+n,std::min(block,appendCount-n));
            dsp.setTimelineState(TimelineState::kIdle);
            for(int n=0;n<firstCount+appendCount;++n)
                require(RvcCaptureTestPeer::captured(dsp,start+n,sampleAt(start+n)),"UI control contention lost a captured input sample");
            // Re-arming at the immediately previous callback position must not be mistaken for pause.
            const int finalOffset=((appendCount-1)/block)*block;
            const int finalSize=appendCount-finalOffset;
            const int64_t finalPos=start+firstCount+finalOffset;
            dsp.beginAppendCapture();
            std::fill(input.begin(),input.end(),0.63f);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,finalSize,1,finalPos,true);
            std::fill(input.begin(),input.end(),0.91f);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,finalSize,1,finalPos,true);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,finalSize,1,finalPos,true);
            dsp.setTimelineState(TimelineState::kIdle);
            for(int n=0;n<finalSize;++n)
                require(RvcCaptureTestPeer::captured(dsp,finalPos+n,0.63f),"same-position append first block was lost or frozen host block overwritten it");
            dsp.beginAppendCapture();
            std::fill(input.begin(),input.end(),0.99f);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,finalSize,1,finalPos,false);
            std::fill(input.begin(),input.end(),0.43f);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,finalSize,1,finalPos,true);
            dsp.setTimelineState(TimelineState::kIdle);
            for(int n=0;n<finalSize;++n)
                require(RvcCaptureTestPeer::captured(dsp,finalPos+n,0.43f),"resume at paused position lost its first block");
            // A new capture invalidates its predecessor immediately, even with no following callback.
            dsp.setTimelineState(TimelineState::kCapture);
            int64_t rangeStart,rangeEnd; dsp.getCaptureRange(rangeStart,rangeEnd);
            require(rangeStart==-1 && rangeEnd==-1,"new capture exposed previous range before playback");
            process(start+firstCount+appendCount,block);
            for(int n=0;n<block;++n)
                require(RvcCaptureTestPeer::captured(dsp,start+firstCount+appendCount+n,sampleAt(start+firstCount+appendCount+n)),"new capture lost its first block");
            require(!RvcCaptureTestPeer::captured(dsp,start,sampleAt(start)),"new capture retained old dry audio");
            dsp.clearCapture(); dsp.getCaptureRange(rangeStart,rangeEnd);
            require(rangeStart==-1 && rangeEnd==-1,"clear exposed previous range before another callback");
            std::cout<<"PASS continuous capture under UI contention and stop/rearm sr="<<rate<<" block="<<block<<" samples="<<firstCount+appendCount<<"\n";
        }
        {
            // Cubase can return its cursor to the play-start position when Space stops playback.
            // A delayed playing flag must not let that return block erase the beginning of the take.
            for (int stopMode=0; stopMode<4; ++stopMode) {
                constexpr int rate=44100, block=128;
                const int64_t origin=31LL*rate+37, punch=origin+133050;
                RvcModelEngine engine; RvcFeatureInjector dsp(engine); dsp.prepare(rate,block);
                auto params=dsp.getParameters(); params.slotAIndexRate=0.2f; dsp.updateParameters(params);
                dsp.setTimelineState(TimelineState::kCapture);
                feed(dsp,origin,8*rate,0.2f,block);
                dsp.setTimelineState(TimelineState::kIdle);
                dsp.beginAppendCapture();
                std::vector<float> input(block,0),output(block);
                // Arming while stopped must not record monitor silence or consume the arm.
                for (int i=0;i<4;++i)
                    dsp.processBlock(input.data(),nullptr,output.data(),nullptr,block,1,punch,false);
                require(dsp.getTimelineState()==TimelineState::kCapture,"stopped append did not stay armed");
                params.slotAIndexRate=0.6f; dsp.updateParameters(params);
                constexpr int count=240*block-37;
                feed(dsp,punch,count,0.6f,block);
                int64_t stopPos=punch;
                if (stopMode==2) stopPos=punch+((count-1)/block)*block; // Frozen last block with playing still set.
                if (stopMode==3) stopPos=punch+count+block; // Seek forward, not contiguous recording.
                dsp.pushAutomationPoint(8,0,0.9f);
                dsp.processBlock(input.data(),nullptr,output.data(),nullptr,block,1,stopPos,stopMode!=0);
                // A later flag or another press of Space must not re-open the completed take.
                for (int i=0;i<6;++i)
                    dsp.processBlock(input.data(),nullptr,output.data(),nullptr,block,1,punch+i*block,true);
                int64_t start,end; dsp.getCaptureRange(start,end);
                require(start==origin && end==origin+8*rate,"stop/return changed the old capture range");
                for (int i=0;i<count;++i)
                    require(RvcCaptureTestPeer::captured(dsp,punch+i,0.6f),"stop/return overwrote an accepted append sample");
                require(RvcCaptureTestPeer::captured(dsp,punch-1,0.2f) &&
                    RvcCaptureTestPeer::captured(dsp,punch+count,0.2f),"stop/return changed untouched dry audio");
                require(RvcCaptureTestPeer::capturedIndex(dsp,punch+block,0.6f),"stop/return overwrote recorded automation");
                require(dsp.getTimelineState()==TimelineState::kIdle,"stop/return did not finish append pass");
                // A new explicit append arm can record over the same position again.
                dsp.beginAppendCapture(); feed(dsp,punch,block,0.4f,block);
                dsp.processBlock(input.data(),nullptr,output.data(),nullptr,block,1,punch,false);
                require(RvcCaptureTestPeer::captured(dsp,punch,0.4f),"explicit rearm failed after transport stop");
                std::cout<<"PASS Cubase append stop/return mode="<<stopMode<<" sr=44100 block=128\n";
            }
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine); dsp.prepare(44100,128);
            dsp.setTimelineState(TimelineState::kCapture);
            feed(dsp,1000,1003,0.3f,128);
            std::vector<float> silence(128),output(128);
            dsp.processBlock(silence.data(),nullptr,output.data(),nullptr,128,1,1000,false);
            require(dsp.getTimelineState()==TimelineState::kIdle,"new capture did not end on explicit host stop");
            feed(dsp,1000,128,0.9f,128);
            require(RvcCaptureTestPeer::captured(dsp,1000,0.3f) && RvcCaptureTestPeer::captured(dsp,2002,0.3f),
                "host restart changed completed new capture or lost short final block");
            std::cout<<"PASS new capture explicit stop preserves first and short final block\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            dsp.setCaptureRange(0,48000); dsp.setTimelineState(TimelineState::kActive);
            require(dsp.getTimelineState()==TimelineState::kIdle,"empty restored metadata became Active");
            std::vector<float> input(512,0.25f),output(512);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,512,1,12000,true);
            require(output==input,"empty restored cache muted dry audio");
            std::cout<<"PASS empty restored capture remains dry\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,0,144000);
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,0,24000,0.7f); feed(dsp,96000,48000,0.7f);
            clearObserved(); generate(dsp);
            std::lock_guard<std::mutex> lock(observedMutex);
            require(observedInputs.size()==1,"unexpected gap synthesis count");
            require(std::abs(observedInputs[0][16000])<1e-7f,"old capture leaked into gap");
            require(std::abs(observedInputs[0][800]-0.7f)<1e-4f,"new capture sample lost");
            std::cout<<"PASS recapture holes contain silence\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            const int64_t base = 240 * 48000 - 24000; // Both dry and automation rings wrap in this phrase.
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,base,48000,0.2f);
            dsp.setTimelineState(TimelineState::kIdle);
            dsp.beginAppendCapture();
            std::vector<float> pausedIn(512,0.99f), pausedOut(512);
            dsp.processBlock(pausedIn.data(),nullptr,pausedOut.data(),nullptr,512,1,base-48000,false);
            int64_t start,end; dsp.getCaptureRange(start,end);
            require(start==base && end==base+48000,"arming append or stopped playback changed old bounds");
            require(pausedIn==pausedOut,"armed append should pass dry audio through");
            require(dsp.getVisualTelemetry().isAppendCapture && !dsp.getVisualTelemetry().isCapturingNow,"append armed telemetry missing");
            feed(dsp,base-48000,24000,0.4f); // Prepend, preserving the gap.
            dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
            feed(dsp,base+96000,48000,0.6f); // Append after the old range.
            dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
            feed(dsp,base+12000,12000,0.8f); // Punch into the original capture.
            dsp.setTimelineState(TimelineState::kIdle);
            dsp.getCaptureRange(start,end);
            require(start==base-48000 && end==base+144000,"append did not union project-time bounds");
            dsp.setTimelineState(TimelineState::kActive);
            require(dsp.getTimelineState()==TimelineState::kIdle,"append activated stale wet cache before Generate");
            clearObserved(); generate(dsp);
            {
                std::lock_guard<std::mutex> lock(observedMutex);
                require(observedInputs.size()==1,"merged phrase should use one inference span");
                const auto& input=observedInputs[0];
                auto at=[&](double sec) { return input[static_cast<size_t>(sec*16000)]; };
                require(std::abs(at(0.25)-0.4f)<1e-4f,"prepended audio missing");
                require(std::abs(at(0.75))<1e-7f,"prepend hole should remain silent");
                require(std::abs(at(1.1)-0.2f)<1e-4f && std::abs(at(1.8)-0.2f)<1e-4f,"overlap erased untouched original audio");
                require(std::abs(at(1.4)-0.8f)<1e-4f,"latest overlapping pass did not replace dry audio");
                require(std::abs(at(2.5))<1e-7f,"append hole should remain silent");
                require(std::abs(at(3.5)-0.6f)<1e-4f,"appended audio missing");
            }
            // A second empty append pass must retain every prior sample.
            dsp.beginAppendCapture(); dsp.setTimelineState(TimelineState::kIdle);
            clearObserved(); generate(dsp);
            {
                std::lock_guard<std::mutex> lock(observedMutex);
                require(observedInputs.empty(),"unchanged empty append unnecessarily regenerated cached audio");
                require(dsp.getVisualTelemetry().appendReusedCache,"empty append did not reuse successful cache");
            }
            auto params=dsp.getParameters(); params.rmsMixRate=0; dsp.updateParameters(params);
            std::vector<float> silent(512,0),gapOutput(512);
            dsp.reset(); feed(dsp,base-48000,10000,0);
            dsp.processBlock(silent.data(),nullptr,gapOutput.data(),nullptr,512,1,base+72000,true);
            require(std::all_of(gapOutput.begin(),gapOutput.end(),[](float x){return std::abs(x)<1e-7f;}),"generated append hole emitted neural audio");
            std::cout<<"PASS append before/after, ring wrap, silence gaps, overlap, armed stop and Generate\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            const int64_t base=48000*800+137;
            dsp.setTimelineState(TimelineState::kCapture);
            dsp.pushAutomationPoint(8,0,0.2f); feed(dsp,base,48000,0.2f);
            dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
            dsp.pushAutomationPoint(8,0,0.4f); feed(dsp,base-48000,48000,0.4f);
            dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
            dsp.pushAutomationPoint(8,0,0.8f); feed(dsp,base+12000,12000,0.8f);
            dsp.setTimelineState(TimelineState::kIdle); clearObserved(); generate(dsp);
            std::lock_guard<std::mutex> lock(observedMutex);
            const auto& curve=observedRequests[0].automationCurves4Col;
            auto at=[&](size_t frame) {return curve[frame*4+2];};
            require(std::abs(at(25)-0.4f)<1e-6f,"prepend automation missing");
            require(std::abs(at(110)-0.2f)<1e-6f && std::abs(at(180)-0.2f)<1e-6f,"overlap erased untouched automation");
            require(std::abs(at(140)-0.8f)<1e-6f,"overlap failed to replace automation");
            std::cout<<"PASS append preserves and replaces automation at non-aligned project positions\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            const int64_t base=48000*300;
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,base,48000,0.2f);
            dsp.setTimelineState(TimelineState::kIdle); dsp.beginAppendCapture();
            feed(dsp,base+240*48000,512,0.9f);
            dsp.setTimelineState(TimelineState::kIdle);
            int64_t start,end; dsp.getCaptureRange(start,end);
            require(start==base && end==base+48000,"capacity overflow changed existing capture bounds");
            require(dsp.getVisualTelemetry().captureCapacityExceeded,"capacity failure not retained after stop");
            clearObserved(); generate(dsp);
            {
                std::lock_guard<std::mutex> lock(observedMutex);
                require(std::abs(observedInputs[0][8000]-0.2f)<1e-4f,"capacity overflow evicted previous samples");
            }
            dsp.beginAppendCapture(); feed(dsp,base-240*48000,512,0.9f);
            require(dsp.getVisualTelemetry().captureCapacityExceeded,"prepend capacity overflow not reported");
            dsp.getCaptureRange(start,end);
            require(start==base && end==base+48000,"prepend overflow changed existing bounds");
            dsp.clearCapture(); require(!dsp.getVisualTelemetry().captureCapacityExceeded,"clear did not reset capacity error");
            std::cout<<"PASS capacity failure preserves original audio and reports both directions\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,0,48000,0.2f);
            synthesisEntered.store(false); holdSynthesis.store(true);
            dsp.triggerGenerate();
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (!synthesisEntered.load() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const bool entered=synthesisEntered.load();
            if (!entered) holdSynthesis.store(false);
            require(entered,"stale-worker fixture did not reach synthesis");
            dsp.beginAppendCapture(); feed(dsp,48000,48000,0.4f);
            const bool reportedIdleWhileCapturing = dsp.waitForSynthesisIdle(5);
            holdSynthesis.store(false);
            require(!reportedIdleWhileCapturing,"capture incorrectly satisfied synthesis idle wait");
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(dsp.getTimelineState()==TimelineState::kCapture,"old inference result activated ongoing append");
            require(dsp.getVisualTelemetry().cachedChunksCount==0,"old inference result counted as appended cache");
            dsp.setTimelineState(TimelineState::kIdle);
            const auto stoppedStats=dsp.getVisualTelemetry();
            require(stoppedStats.totalActiveChunks==1 && stoppedStats.cachedChunksCount==0 && stoppedStats.cacheCoverageRatio==0,
                "stopped append presented predecessor's completed cache");
            clearObserved(); generate(dsp);
            {
                std::lock_guard<std::mutex> lock(observedMutex);
                require(std::abs(observedInputs[0][8000]-0.2f)<1e-4f && std::abs(observedInputs[0][24000]-0.4f)<1e-4f,"new generation omitted old or appended audio");
            }
            dsp.setTimelineState(TimelineState::kIdle);
            require(dsp.getVisualTelemetry().cacheCoverageRatio==1.0f,"Idle hid a valid generated cache");
            require(!dsp.waitForSynthesisIdle(5),"Idle state incorrectly satisfied synthesis idle wait");
            dsp.setTimelineState(TimelineState::kActive);
            require(dsp.getTimelineState()==TimelineState::kActive,"Active off/on discarded valid generated cache");
            std::cout<<"PASS obsolete inference cannot publish or activate a newer capture\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine); dsp.prepare(48000,960);
            dsp.setTimelineState(TimelineState::kCapture);
            std::vector<float> input(960,0.2f),output(960);
            dsp.pushAutomationPoint(8,0,0.1f); dsp.pushAutomationPoint(8,480,0.9f);
            dsp.pushAutomationPoint(1,700,0.4f);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,960,1,0,true);
            require(std::abs(dsp.getParameters().injectDepth-0.4f)<1e-6f &&
                    std::abs(dsp.getParameters().dryWetMix-0.4f)<1e-6f,"inject/drywet automation lost linkage");
            feed(dsp,960,47040); clearObserved(); generate(dsp);
            {
                std::lock_guard<std::mutex> lock(observedMutex);
                const auto& curve=observedRequests[0].automationCurves4Col;
                require(curve.size()==400,"capture automation frame count incorrect");
                require(std::abs(curve[2]-0.1f)<1e-6f && std::abs(curve[6]-0.9f)<1e-6f,"sampleOffset was applied before its sample");
            }
            auto parameters=dsp.getParameters(); parameters.consonantProtect=0.17f; dsp.updateParameters(parameters);
            clearObserved(); generate(dsp);
            {std::lock_guard<std::mutex> lock(observedMutex); require(std::abs(observedRequests[0].protect-0.17f)<1e-6f,"protect is not forwarded");}
            parameters.consonantProtect=0.25f; dsp.updateParameters(parameters);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(dsp.getVisualTelemetry().isPendingRegenerate,"protect change did not invalidate generation");
            std::cout<<"PASS capture sample offsets, linked controls and protect\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,0,48000); generate(dsp);
            auto parameters=dsp.getParameters(); parameters.rmsMixRate=0; dsp.updateParameters(parameters);
            std::vector<float> input(512,0),baseline(512),automated(512);
            dsp.reset(); feed(dsp,0,10000,0);
            dsp.processBlock(input.data(),nullptr,baseline.data(),nullptr,512,1,10000,true);
            dsp.reset(); feed(dsp,0,10000,0);
            dsp.pushAutomationPoint(10,256,6);
            dsp.processBlock(input.data(),nullptr,automated.data(),nullptr,512,1,10000,true);
            require(std::equal(baseline.begin(),baseline.begin()+256,automated.begin()),"realtime gain automation applied early");
            require(automated[256]>baseline[256] && automated.back()>baseline.back()*1.1f,"realtime gain sample offset not applied");
            parameters=dsp.getParameters(); parameters.outputGainDb=0; dsp.updateParameters(parameters);
            dsp.reset(); feed(dsp,0,10000,0);
            std::fill(input.begin(),input.end(),0.0f);
            dsp.processBlock(input.data(),nullptr,baseline.data(),nullptr,512,1,12000,true);
            dsp.reset(); feed(dsp,0,10000,0);
            std::fill(input.begin(),input.end(),0.9f);
            dsp.processBlock(input.data(),nullptr,automated.data(),nullptr,512,1,12000,true);
            require(std::abs(automated[0]-baseline[0])<1e-6f,"seek leaked the live dry input into fully wet playback");
            std::cout<<"PASS realtime gain automation begins exactly at sampleOffset\n";
        }
        {
            RvcModelEngine engine; RvcFeatureInjector dsp(engine); dsp.setTimelineState(TimelineState::kCapture);
            for(int frame=0;frame<1600;++frame) {
                dsp.pushAutomationPoint(8,0,frame/2000.0f); feed(dsp,frame*480,480,0.2f,480);
            }
            clearObserved(); generate(dsp);
            std::lock_guard<std::mutex> lock(observedMutex);
            require(observedRequests.size()==2,"context test expected two synthesis spans");
            for(size_t i=0;i<2;++i)
                require(observedRequests[i].automationCurves4Col.size()/4==(observedInputs[i].size()+159)/160,"curve does not cover submitted context");
            require(std::abs(observedRequests[1].automationCurves4Col[2]-0.575f)<1e-5f,"left context curve has wrong timeline origin");
            std::cout<<"PASS automation includes left and right context at 100 Hz\n";
        }
        for(float hz : {110.0f,220.0f,440.0f}) {
            toneHz=hz; RvcModelEngine engine; RvcFeatureInjector dsp(engine);
            dsp.setTimelineState(TimelineState::kCapture); feed(dsp,0,144000); generate(dsp);
            for(float pitch : {0.0f,-12.0f,12.0f}) {
                auto output=render(dsp,pitch);
                auto first=std::find_if(output.begin(),output.end(),[](float x){return std::abs(x)>1e-4f;});
                auto last=std::find_if(output.rbegin(),output.rend(),[](float x){return std::abs(x)>1e-4f;});
                const auto begin=first-output.begin(); const auto end=output.size()-(last-output.rbegin());
                require(begin==33600,"pitch changed the phrase onset"); require(end==105600,"pitch changed phrase endpoint");
                const double expected=hz*std::pow(2.0,pitch/12.0);
                const double detected=dominantHz(output,expected);
                std::cout<<"pitch f0="<<hz<<" st="<<pitch<<" onset="<<begin<<" end="<<end<<" detected="<<detected<<" expected="<<expected<<"\n";
                require(std::abs(detected-expected)<expected*0.06,"pitch frequency ratio incorrect");
                require(dsp.getLatencySamples()==0,"cached lookahead should require no PDC");
                if(hz==220 && pitch==12) {
                    auto smallBlockOutput=render(dsp,pitch,127);
                    require(output==smallBlockOutput,"pitch output depends on host buffer size");
                }
            }
            // Seek into the same cache after a prior render; the previous phrase must not bleed in.
            std::vector<float> input(512,0),output(512);
            dsp.processBlock(input.data(),nullptr,output.data(),nullptr,512,1,10000,true);
            require(std::all_of(output.begin(),output.end(),[](float x){return std::abs(x)<1e-7f;}),"seek replayed stale pitch history");
            if (hz==110) {
                dsp.prepare(44100,512);
                require(dsp.getTimelineState()==TimelineState::kIdle,"sample rate change retained an invalid Active cache");
                int64_t start,end; dsp.getCaptureRange(start,end);
                require(start==-1 && end==-1,"sample rate change retained capture bounds");
            }
        }
        std::cout<<"PASS pitch onset, endpoint, frequency and seek\n";
    } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<"\n"; return 1; }
}
