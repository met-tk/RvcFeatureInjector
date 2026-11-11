#if defined(RVC_LEGACY_CAPTURE)
#include "../artifacts/append-investigation-1.1.8/startup-legacy-src/src/dsp/RvcFeatureInjector.h"
#else
#include "../src/dsp/RvcFeatureInjector.h"
#endif
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using namespace rvc_vst;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void feed(RvcFeatureInjector& dsp,int64_t position,int samples,float value,bool playing=true,int block=128) {
    std::vector<float> input(block,value),output(block);
    for(int offset=0;offset<samples;offset+=block)dsp.processBlock(input.data(),nullptr,output.data(),nullptr,
        std::min(block,samples-offset),1,position>=0?position+offset:-1,playing);
}
void stopped(RvcFeatureInjector& dsp,int64_t position) { feed(dsp,position,128,0,false); }
std::vector<float> readFloat(const std::filesystem::path& file) {
    std::ifstream input(file,std::ios::binary);require(bool(input),"Cannot open replay raw");
    const auto bytes=std::filesystem::file_size(file);require(bytes%4==0,"Invalid replay raw size");
    std::vector<float> result(bytes/4);input.read(reinterpret_cast<char*>(result.data()),bytes);
    require(bool(input),"Truncated replay raw");return result;
}
template<class T>void save(const std::filesystem::path& file,const std::vector<T>& values) {
    std::ofstream output(file,std::ios::binary);require(bool(output),"Cannot save replay evidence");
    output.write(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(T));require(bool(output),"Replay evidence write failed");
}
}
namespace rvc_vst {
struct RvcCaptureTestPeer {
    static bool valid(RvcFeatureInjector& d,int64_t project) {
        int64_t start,end;d.getCaptureRange(start,end);
        return project>=start&&project<end&&d.hasCapturedSample(static_cast<size_t>(project-start),d.captureGeneration_.load(),start);
    }
    static float raw(RvcFeatureInjector& d,int64_t project) {
        int64_t start,end;d.getCaptureRange(start,end);
        return project>=start&&project<end?d.readCapturedSample(static_cast<size_t>(project-start),d.captureGeneration_.load(),start):0;
    }
    static bool started(RvcFeatureInjector& d) { return d.capturePassStarted_; }
    static float index(RvcFeatureInjector& d,int64_t project) {
        const auto frame=d.automationFrameAt(project);
        return d.automationTimeline_[frame%RvcFeatureInjector::kAutomationCacheFrames].slotAIndexRate.load();
    }
};
struct RvcModelEngine::Impl {};
RvcModelEngine::RvcModelEngine()=default;
RvcModelEngine::~RvcModelEngine()=default;
void RvcModelEngine::cancelPendingWork() noexcept {}
void RvcModelEngine::resumePendingWork() noexcept {}
bool RvcModelEngine::syncDaemonSlotsIfNeeded() { return true; }
RvcModelSlotInfo RvcModelEngine::getSlotInfo(int) const { return {}; }
std::string RvcModelEngine::getLoadingStatusText() const { return {}; }
bool RvcModelEngine::synthesizeRvcChunk(const float*,size_t,const RvcChunkSynthRequest&,RvcChunkSynthResult&,
    const std::function<void(float,const std::string&)>&) { return false; }
}

void synthetic() {
    constexpr int64_t origin=10000,anchor=13078;
    for(int rate:{44100,48000,96000}) {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(rate,128);
        auto parameters=dsp.getParameters();parameters.slotAIndexRate=.3f;dsp.updateParameters(parameters);
        dsp.setTimelineState(TimelineState::kCapture);feed(dsp,origin,12000,.2f);dsp.setTimelineState(TimelineState::kIdle);
        stopped(dsp,anchor);dsp.beginAppendCapture();
        for(int64_t position=anchor-3*128-78;position+128<=anchor;position+=128) {
            feed(dsp,position,128,1e-10f);
            require(!RvcCaptureTestPeer::started(dsp),"Pre-roll started a take");
            require(dsp.getTimelineState()==TimelineState::kCapture,"Pre-roll consumed armed Capture");
        }
        const int64_t cross=anchor-78;
        feed(dsp,cross,128,1e-10f);
        parameters.slotAIndexRate=.8f;dsp.updateParameters(parameters);
        feed(dsp,cross+128,128,0); // Deliberate valid digital silence, never filtered by level.
        feed(dsp,cross+256,128,.6f);
        require(dsp.getTimelineState()==TimelineState::kCapture,"Partial first block broke host block continuity");
        for(int64_t i=origin;i<anchor;++i)require(RvcCaptureTestPeer::raw(dsp,i)==.2f,"Append pre-roll changed earlier dry");
        for(int64_t i=anchor;i<cross+128;++i)require(RvcCaptureTestPeer::raw(dsp,i)==1e-10f,"Valid near-silence after anchor was dropped");
        for(int64_t i=cross+128;i<cross+256;++i)require(RvcCaptureTestPeer::raw(dsp,i)==0&&RvcCaptureTestPeer::valid(dsp,i),"Valid silence did not overwrite old voice");
        for(int64_t i=cross+256;i<cross+384;++i)require(RvcCaptureTestPeer::raw(dsp,i)==.6f,"Post-anchor edit was hidden");
        require(RvcCaptureTestPeer::index(dsp,anchor-1)==.3f,"Pre-roll altered existing automation");
        feed(dsp,cross+384,2*(rate/100)+128,.6f);
        require(RvcCaptureTestPeer::index(dsp,anchor-1)==.3f,"Later block overwrote the old partial automation frame");
        require(RvcCaptureTestPeer::index(dsp,anchor+rate/100)==.8f,"Valid automation after boundary frame was ignored");
        stopped(dsp,anchor);require(dsp.getTimelineState()==TimelineState::kIdle,"Stop did not finish real take");
        std::cout<<"PASS stopped Append clips pre-roll and preserves real near-silence/silence/edit sr="<<rate<<"\n";
    }
    {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);
        stopped(dsp,anchor);dsp.setTimelineState(TimelineState::kCapture);
        feed(dsp,anchor-128,128,0);int64_t start,end;dsp.getCaptureRange(start,end);
        require(start==-1&&end==-1&&!RvcCaptureTestPeer::started(dsp),"New Capture included pre-roll bounds");
        feed(dsp,anchor,128,.4f);dsp.getCaptureRange(start,end);
        require(start==anchor&&end==anchor+128,"New Capture did not start at armed cursor");
        std::cout<<"PASS new Capture clips pre-roll bounds\n";
    }
    {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);
        stopped(dsp,1000);dsp.setTimelineState(TimelineState::kCapture);
        feed(dsp,1000,128,.3f);feed(dsp,500,128,.6f);
        int64_t start,end;dsp.getCaptureRange(start,end);
        require(start==500&&end==1128&&RvcCaptureTestPeer::raw(dsp,500)==.6f,
            "New Capture playing seek was incorrectly constrained by its startup floor");
        std::cout<<"PASS new Capture playing seek can prepend after its first accepted block\n";
    }
    {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);
        stopped(dsp,anchor);dsp.setTimelineState(TimelineState::kCapture);stopped(dsp,anchor+1000);
        feed(dsp,anchor+900,128,.4f);int64_t start,end;dsp.getCaptureRange(start,end);
        require(start==anchor+1000&&end==anchor+1028,"Armed stopped seek did not update anchor");
        std::cout<<"PASS stopped cursor seek updates capture floor\n";
    }
    {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);
        stopped(dsp,anchor);feed(dsp,anchor+500,128,.2f);dsp.beginAppendCapture();feed(dsp,anchor-500,128,.7f);
        require(RvcCaptureTestPeer::raw(dsp,anchor-500)==.7f,"Playing arm reused a stale stopped anchor");
        std::cout<<"PASS already-playing Append records its next callback\n";
    }
    {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);
        stopped(dsp,anchor);dsp.setTimelineState(TimelineState::kCapture);feed(dsp,-1,128,.5f);
        int64_t start,end;dsp.getCaptureRange(start,end);require(start==0&&end==128,"Missing context lost internal fallback capture");
        require(RvcCaptureTestPeer::raw(dsp,0)==.5f,"Missing context inherited an unusable floor");
        std::cout<<"PASS missing timeline context retains fallback\n";
    }
    {
        RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);stopped(dsp,anchor);
        dsp.prepare(48000,128);dsp.setTimelineState(TimelineState::kCapture);feed(dsp,100,128,.8f);
        require(RvcCaptureTestPeer::raw(dsp,100)==.8f,"Sample-rate prepare retained stale stopped timeline");
        std::cout<<"PASS sample-rate prepare clears old host position\n";
    }
}

void realReplay(const std::filesystem::path& event,const std::filesystem::path& output) {
    constexpr int64_t origin=9151493,oldEnd=9789573,newEnd=10040926,anchor=9740588,first=9735774;
    auto old=readFloat(event/L"old_dry.f32"),incoming=readFloat(event/L"new_dry.f32");
    require(old.size()==oldEnd-origin&&incoming.size()==newEnd-origin,"Unexpected real event range");
    RvcModelEngine engine;RvcFeatureInjector dsp(engine);dsp.prepare(44100,128);
    std::vector<float> out(128),in(128);dsp.setTimelineState(TimelineState::kCapture);
    for(size_t at=0;at<old.size();at+=128) {
        const auto n=static_cast<int>(std::min<size_t>(128,old.size()-at));
        dsp.processBlock(old.data()+at,nullptr,out.data(),nullptr,n,1,origin+at,true);
    }
    dsp.setTimelineState(TimelineState::kIdle);stopped(dsp,anchor);dsp.beginAppendCapture();
    std::ifstream trace(event/L"host-trace-before_generate.csv");require(bool(trace),"Missing real host trace");
    std::string line;std::getline(trace,line);size_t blocks=0;bool recording=false;
    while(std::getline(trace,line)) {
        std::istringstream row(line);std::vector<std::string> fields;std::string field;
        while(std::getline(row,field,','))fields.push_back(field);
        require(fields.size()>=20,"Malformed host trace");
        const int state=std::stoi(fields[3]),playing=std::stoi(fields[6]),n=std::stoi(fields[13]);
        const auto position=std::stoll(fields[7]);
        if(state==1&&playing&&position==first)recording=true;
        if(!recording)continue;
        require(n>0&&n<=128,"Unexpected host replay block size");
        if(playing&&position>=first&&position+n<=newEnd)std::copy_n(incoming.data()+position-origin,n,in.data());
        else std::fill(in.begin(),in.end(),0.0f);
        dsp.processBlock(in.data(),nullptr,out.data(),nullptr,n,1,position,playing!=0);
        if(playing)++blocks;
        else break;
    }
    require(blocks==2384,"Actual host trace was not completely replayed");
    std::vector<float> fixed(incoming.size());std::vector<unsigned char> valid(incoming.size());
    for(size_t i=0;i<fixed.size();++i) {fixed[i]=RvcCaptureTestPeer::raw(dsp,origin+i);valid[i]=RvcCaptureTestPeer::valid(dsp,origin+i)?1:0;}
    size_t beforeChanged=0,afterChanged=0;
    for(int64_t i=origin;i<anchor;++i)beforeChanged+=fixed[i-origin]!=old[i-origin];
    for(int64_t i=anchor;i<newEnd;++i)afterChanged+=fixed[i-origin]!=incoming[i-origin];
#if defined(RVC_LEGACY_CAPTURE)
    require(beforeChanged==4813,"Legacy did not reproduce the 4813 overwritten real samples");
    const char* name="legacy-dry";
#else
    require(beforeChanged==0,"Fixed replay changed a sample before the real stopped cursor");
    require(afterChanged==0,"Fixed replay altered valid post-anchor input");
    const char* name="fixed-dry";
#endif
    std::filesystem::create_directories(output);save(output/(std::string(name)+".f32"),fixed);
    save(output/(std::string(name)+"-validity.u8"),valid);
    old.resize(incoming.size(),0);std::vector<unsigned char> oldValid(incoming.size(),0);std::fill_n(oldValid.begin(),oldEnd-origin,1);
    save(output/L"old-dry.f32",old);save(output/L"old-validity.u8",oldValid);save(output/L"bad-dry.f32",incoming);
    std::ofstream metadata(output/L"replay-metadata.json");metadata<<"{\"sample_rate\":44100,\"start_sample\":"<<origin
        <<",\"old_end_sample\":"<<oldEnd<<",\"end_sample\":"<<newEnd<<",\"append_first_host_sample\":"<<first
        <<",\"boundary_sample\":"<<anchor<<",\"host_blocks\":"<<blocks<<"}\n";
    std::cout<<"PASS real host replay blocks="<<blocks<<" changed_before_anchor="<<beforeChanged<<" changed_after_anchor="<<afterChanged<<"\n";
}
int main(int argc,char** argv) {
    try {
#if !defined(RVC_LEGACY_CAPTURE)
        synthetic();
#endif
        if(argc==3)realReplay(std::filesystem::u8path(argv[1]),std::filesystem::u8path(argv[2]));
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
