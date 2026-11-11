#include "../src/dsp/RvcFeatureInjector.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
using namespace rvc_vst;
namespace {
constexpr double pi=3.14159265358979323846;
std::atomic<int> mockCalls{0};
float sourceAt(int at,int sr) {
    const int q=at%(sr/2);
    if(q==0) return 0.8f;
    if(q==1) return -0.8f;
    if(q>=44 && q<44+sr/200) {
        const double t=static_cast<double>(q-44)/sr;
        return static_cast<float>(0.15*std::sin(2*pi*6100*t)+0.15*std::sin(2*pi*8700*t+0.7));
    }
    return static_cast<float>(0.08*std::sin(2*pi*220*at/sr));
}
}
namespace rvc_vst {
struct RvcCaptureTestPeer {
    static void injectWet(RvcFeatureInjector& d,int samples,int sr) {
        for(int i=0;i<samples;++i) {
            const auto value=sourceAt(i,sr);
            d.wetSlotACache_[i].store(value);
            d.wetSlotBCache_[i].store(value);
        }
    }
};
struct RvcModelEngine::Impl {};
RvcModelEngine::RvcModelEngine()=default;
RvcModelEngine::~RvcModelEngine()=default;
void RvcModelEngine::cancelPendingWork() noexcept {}
void RvcModelEngine::resumePendingWork() noexcept {}
bool RvcModelEngine::syncDaemonSlotsIfNeeded() { return true; }
RvcModelSlotInfo RvcModelEngine::getSlotInfo(int) const { RvcModelSlotInfo slot;slot.isLoaded=true;return slot; }
std::string RvcModelEngine::getLoadingStatusText() const { return {}; }
bool RvcModelEngine::synthesizeRvcChunk(const float*,size_t count,const RvcChunkSynthRequest& request,RvcChunkSynthResult& result,
    const std::function<void(float,const std::string&)>&) {
    mockCalls.fetch_add(1);
    const auto n=static_cast<size_t>(std::llround(count*request.targetSampleRate/16000.0));
    result.slotAAudioSamples.assign(n,0.2f);result.slotBAudioSamples=result.slotAAudioSamples;return true;
}
}
int verifyNoOp();
int main() {
    constexpr int sr=44100,block=128,total=2*sr;
    const int64_t origin=31LL*sr+37;
    RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(sr,block);
    auto params=dsp.getParameters();params.rmsMixRate=0;dsp.updateParameters(params);
    std::vector<float> left(block,.2f),right(block,.2f),outL(block),outR(block);
    dsp.setTimelineState(TimelineState::kCapture);
    for(int at=0;at<total;at+=block) {
        dsp.processBlock(left.data(),right.data(),outL.data(),outR.data(),std::min(block,total-at),2,origin+at,true);
    }
    dsp.setTimelineState(TimelineState::kIdle);dsp.triggerGenerate();
    if(!dsp.waitForSynthesisIdle(4000)) return 2;
    RvcCaptureTestPeer::injectWet(dsp,total,sr);
    for(float rms : {0.0f,0.25f,1.0f}) {
        params.rmsMixRate=rms;dsp.updateParameters(params);
        for(int start : {0,sr/2,total-256}) {
            const int count=std::min(2048,total-start);
            std::vector<std::vector<float>> renders;
            for(float live : {-10.0f,0.0f,10.0f}) {
                dsp.reset();
                std::fill(left.begin(),left.end(),live);std::fill(right.begin(),right.end(),live);
                std::vector<float> output;
                for(int at=0;at<count;at+=block) {
                    const int n=std::min(block,count-at);
                    dsp.processBlock(left.data(),right.data(),outL.data(),outR.data(),n,2,origin+start+at,true);
                    output.insert(output.end(),outL.begin(),outL.begin()+n);
                }
                renders.push_back(std::move(output));
            }
            double maxLiveDifference=0,maxCacheError=0,maxPeak=0,maxDelta=0;
            int differing=0;
            for(int i=0;i<count;++i) {
                const float lo=std::min({renders[0][i],renders[1][i],renders[2][i]});
                const float hi=std::max({renders[0][i],renders[1][i],renders[2][i]});
                maxLiveDifference=std::max(maxLiveDifference,static_cast<double>(hi-lo));
                if(lo!=hi) ++differing;
                for(const auto& render:renders) {
                    maxPeak=std::max(maxPeak,static_cast<double>(std::abs(render[i])));
                    maxCacheError=std::max(maxCacheError,static_cast<double>(std::abs(render[i]-sourceAt(start+i,sr))));
                    if(i) maxDelta=std::max(maxDelta,static_cast<double>(std::abs(render[i]-render[i-1])));
                }
            }
            if (differing != 0 || maxLiveDifference != 0 || (rms == 0 && maxCacheError > 2e-7)) return 1;
            std::cout<<"{\"test\":\"wet_isolation\",\"rms_mix\":"<<rms<<",\"start\":"<<start<<",\"samples\":"<<count
                <<",\"max_live_difference\":"<<maxLiveDifference<<",\"differing_samples\":"<<differing
                <<",\"max_cache_error_rms0_only\":"<<maxCacheError<<",\"max_peak\":"<<maxPeak<<",\"max_delta\":"<<maxDelta
                <<",\"first_negative_live\":"<<renders[0][0]<<",\"first_zero_live\":"<<renders[1][0]<<",\"first_positive_live\":"<<renders[2][0]<<"}\n";
        }
    }
    params.rmsMixRate=0;dsp.updateParameters(params);
    for(int mode:{0,1,2}) {
        dsp.reset();std::fill(left.begin(),left.end(),0.0f);std::fill(right.begin(),right.end(),0.0f);
        const int start=mode==2?total-512:sr/2+130;
        for(int at=0;at<512;at+=block)
            dsp.processBlock(left.data(),right.data(),outL.data(),outR.data(),block,2,origin+start+at,true);
        const int64_t stopped=origin+start+384;
        std::vector<float> paused;
        for(int at=0;at<4096;at+=block) {
            dsp.processBlock(left.data(),right.data(),outL.data(),outR.data(),block,2,stopped,mode==1);
            paused.insert(paused.end(),outL.begin(),outL.end());
        }
        double afterTailPeak=0,tailPeak=0,maxDelta=0;
        const int fadeEnd=static_cast<int>(std::ceil(sr*0.003))+1;
        for(int i=0;i<static_cast<int>(paused.size());++i) {
            if(i>=fadeEnd) afterTailPeak=std::max(afterTailPeak,static_cast<double>(std::abs(paused[i])));
            else tailPeak=std::max(tailPeak,static_cast<double>(std::abs(paused[i])));
            if(i) maxDelta=std::max(maxDelta,static_cast<double>(std::abs(paused[i]-paused[i-1])));
        }
        const int restart=sr/2;
        dsp.processBlock(left.data(),right.data(),outL.data(),outR.data(),block,2,origin+restart,true);
        const double restartError=std::abs(outL[0]-sourceAt(restart,sr));
        if (afterTailPeak != 0 || restartError > 2e-7) return 1;
        std::cout<<"{\"test\":\"pause_tail\",\"mode\":"<<mode<<",\"tail_peak\":"<<tailPeak
            <<",\"peak_after_3ms\":"<<afterTailPeak<<",\"paused_max_delta\":"<<maxDelta<<",\"restart_first_sample_error\":"<<restartError<<"}\n";
    }
    return verifyNoOp();
}

int verifyNoOp() {
    constexpr int sr=44100,block=128,total=sr,edit=10001,length=2000;
    const int64_t origin=31LL*sr+37;
    RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(sr,block);
    std::vector<float> source(total,0.2f),left(block),right(block),outL(block),outR(block);
    auto pass=[&](int start,int count) {
        for(int at=0;at<count;at+=block) {
            const int n=std::min(block,count-at);
            for(int i=0;i<n;++i) left[i]=right[i]=source[start+at+i];
            dsp.processBlock(left.data(),right.data(),outL.data(),outR.data(),n,2,origin+start+at,true);
        }
    };
    auto report=[&](const char* scenario,int before) {
        dsp.triggerGenerate();
        if(!dsp.waitForSynthesisIdle(4000)) return false;
        const auto tel=dsp.getVisualTelemetry();
        std::cout<<"{\"scenario\":\""<<scenario<<"\",\"calls_before\":"<<before<<",\"calls_after\":"<<mockCalls.load()
            <<",\"state\":"<<static_cast<int>(dsp.getTimelineState())<<",\"compared\":"<<tel.appendComparedSamples
            <<",\"changed\":"<<tel.appendChangedSamples<<",\"reused\":"<<(tel.appendReusedCache?"true":"false")<<"}\n";
        const bool shouldReuse = std::strcmp(scenario,"matched")==0 || std::strcmp(scenario,"matched_twice")==0 ||
            std::strcmp(scenario,"empty_append")==0 || std::strcmp(scenario,"same_recorded_silence")==0 ||
            std::strcmp(scenario,"two_pending_matched_passes")==0;
        return tel.appendReusedCache == shouldReuse && (mockCalls.load() == before) == shouldReuse;
    };
    dsp.setTimelineState(TimelineState::kCapture);pass(0,total);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("baseline",mockCalls.load())) return 2;
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("matched",mockCalls.load())) return 2;
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("matched_twice",mockCalls.load())) return 2;
    dsp.beginAppendCapture();dsp.setTimelineState(TimelineState::kIdle);
    if(!report("empty_append",mockCalls.load())) return 2;
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    dsp.beginAppendCapture();pass(edit+length,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("two_pending_matched_passes",mockCalls.load())) return 2;
    source[edit]=std::nextafter(source[edit],1.0f);
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("one_ulp_changed",mockCalls.load())) return 2;
    auto params=dsp.getParameters();params.consonantProtect=std::nextafter(params.consonantProtect,1.0f);dsp.updateParameters(params);
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("one_ulp_protect_changed",mockCalls.load())) return 2;
    params.slotAIndexRate=std::nextafter(params.slotAIndexRate,1.0f);dsp.updateParameters(params);
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("one_ulp_index_changed",mockCalls.load())) return 2;
    // Valid recorded silence must replace existing voice rather than be treated as no-op.
    for(int i=edit;i<edit+length;++i) source[i]=0;
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("real_silence_overwrite",mockCalls.load())) return 2;
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("same_recorded_silence",mockCalls.load())) return 2;
    dsp.setTimelineState(TimelineState::kCapture);pass(0,edit);pass(edit+length,total-edit-length);
    dsp.setTimelineState(TimelineState::kIdle);
    if(!report("capture_with_hole",mockCalls.load())) return 2;
    dsp.beginAppendCapture();pass(edit,length);dsp.setTimelineState(TimelineState::kIdle);
    if(!report("new_valid_silence_in_hole",mockCalls.load())) return 2;
    return 0;
}
