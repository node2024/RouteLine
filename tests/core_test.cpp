#include "display.h"
#include "legacy_drawing.h"
#include "path.h"
#include "curve.h"
#include "workshop_geometry.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <zstd.h>

namespace {
template<class T> void append(std::string& out,T value) {out.append(reinterpret_cast<const char*>(&value),sizeof(value));}
void varint(std::string& out,uint64_t n) {while(n>=128) {out+=char((n&127)|128);n>>=7;}out+=char(n);}
void integer(std::string& out,unsigned field,uint64_t n) {varint(out,field<<3);varint(out,n);}
void text(std::string& out,unsigned field,const std::string& bytes) {varint(out,(field<<3)|2);varint(out,bytes.size());out+=bytes;}
void real(std::string& out,unsigned field,float value) {varint(out,(field<<3)|5);append(out,value);}
std::string compress(const std::string& raw) {
    std::string out(ZSTD_compressBound(raw.size()),'\0');auto n=ZSTD_compress(out.data(),out.size(),raw.data(),raw.size(),3);assert(!ZSTD_isError(n));out.resize(n);return out;
}
void section(std::string& out,const std::string& raw,uint64_t count,unsigned version,bool concatenate=false) {
    auto bytes=concatenate?compress(raw.substr(0,raw.size()/2))+compress(raw.substr(raw.size()/2)):compress(raw);
    if(version>=7) {append(out,uint64_t(bytes.size()));append(out,uint64_t(raw.size()));append(out,count);}
    else {append(out,uint32_t(bytes.size()));append(out,uint32_t(raw.size()));append(out,uint32_t(count));}
    out+=bytes;
}
std::string timer(unsigned version,unsigned tick,unsigned type,float duration=1) {
    std::string e;
    if(version>=6) {
        integer(e,1,0);integer(e,2,tick);integer(e,3,0);
        std::string t;integer(t,1,type);integer(t,2,1);real(t,3,duration);text(e,4,t);
        std::string out;append(out,uint32_t(e.size()));return out+e;
    }
    append(e,uint32_t(0));append(e,uint32_t(tick));append(e,uint32_t(type));append(e,int32_t(1));append(e,duration);e.resize(172,0);return e;
}
std::string positionEvent(unsigned version,unsigned tick,line::Vec3 origin,bool checkpointSave=false,unsigned reason=2) {
    std::string e;
    if(version>=6) {
        std::string payload,position;
        real(position,1,origin.x);real(position,2,origin.y);real(position,3,origin.z);
        integer(payload,1,checkpointSave?0:reason);
        integer(payload,2,1);text(payload,checkpointSave?3:4,position);
        integer(e,1,checkpointSave?4:3);integer(e,2,tick);text(e,checkpointSave?8:7,payload);
        std::string out;append(out,uint32_t(e.size()));return out+e;
    }
    append(e,uint32_t(3));append(e,uint32_t(tick));append(e,uint32_t(1));append(e,origin);
    e.resize(172,0);return e;
}
struct Motion {
    line::Vec3 preVelocity,postVelocity;
    float preHeight=0;
    line::Vec3 postOffset;
    uint32_t preGround=0,postGround=0,preButtons=0,postButtons=0;
    uint8_t preDuck=0,postDuck=0;
    uint8_t preMoveType=2,postMoveType=2;
    float preAmount=0,postAmount=0;
    int32_t landedTick=0;
    float landedFraction=0;
    bool inherit=false,omitMoveType=false;
    std::optional<line::Vec3> preOrigin;
};
std::string fixture(unsigned version,bool teleport=false,bool styles=false,bool shortTeleport=false,
                    const std::vector<Motion>* motions=nullptr,const std::vector<std::string>& positionEvents={}) {
    std::string h,run,map,mode;
    integer(h,1,version);integer(h,2,2);text(map,1,"kz_test");text(h,4,map);
    text(run,1,"Main Course");text(mode,1,"Classic");text(mode,2,"CKZ");text(run,2,mode);real(run,4,1);integer(run,5,0);
    if(styles) text(run,3,mode);
    text(h,16,run);
    std::string out;append(out,uint32_t(h.size()));out+=h;
    std::string ticks;
    const auto tickCount=motions?uint32_t(motions->size()):7u;
    for(uint32_t i=0;i<tickCount;++i) {
        const auto m=motions?motions->at(i):Motion{};
        uint64_t flags=(1ULL<<10)|(1ULL<<38);
        if(!m.inherit) flags|=(1ULL<<11)|(1ULL<<13)|(1ULL<<18)|(1ULL<<21)|(1ULL<<22)|(1ULL<<23)
            |(1ULL<<25)|(1ULL<<27)|(1ULL<<32)|(1ULL<<35)|(1ULL<<36)|(1ULL<<37);
        if(m.omitMoveType) flags&=~((1ULL<<23)|(1ULL<<37));
        if(m.postOffset.dot(m.postOffset)>0) flags|=1ULL<<24;
        if(i==0) flags|=1;
        // Exercise the weapon/modern-jump layout, including v2's overlapping mask.
        if(i==0) {flags|=1ULL<<39;if(version>=2) flags|=version==2?(3ULL<<40):(7ULL<<40);}
        if(version>=2&&motions&&(i==0||m.landedTick!=motions->at(i-1).landedTick||m.landedFraction!=motions->at(i-1).landedFraction))
            flags|=1ULL<<(version==2?41:42);
        append(ticks,flags);
        if(i==0) append(ticks,uint32_t(98));
        if(flags&(1ULL<<39)) append(ticks,int32_t(-1));
        const auto preOrigin=m.preOrigin.value_or(line::Vec3{
            float(i*16+(teleport&&i>=4?500:0)+(shortTeleport&&i>=5?16:0)),0,m.preHeight});
        append(ticks,preOrigin);
        if(!m.inherit) {
            append(ticks,m.preVelocity);append(ticks,m.preButtons);append(ticks,m.preAmount);append(ticks,m.preDuck);append(ticks,m.preGround);
            if(!m.omitMoveType) append(ticks,m.preMoveType);
        }
        if(flags&(1ULL<<24)) append(ticks,preOrigin+m.postOffset);
        if(!m.inherit) {
            append(ticks,m.postVelocity);append(ticks,m.postButtons);append(ticks,m.postAmount);append(ticks,m.postDuck);append(ticks,m.postGround);
            if(!m.omitMoveType) append(ticks,m.postMoveType);
        }
        append(ticks,int32_t(0));append(ticks,int32_t(0));append(ticks,int32_t(teleport&&i>=4?1:0));
        if(version>=2) {
            unsigned first=version==2?39:40;
            if(flags&(1ULL<<first)) ticks.append(8,'\0');
            if(flags&(1ULL<<(first+1))) ticks.append(8,'\0');
            if(flags&(1ULL<<(first+2))) {append(ticks,m.landedTick);append(ticks,m.landedFraction);ticks.append(12,'\0');}
        }
    }
    section(out,ticks,tickCount,version,version>=7);section(out,{},0,version);section(out,{},0,version);section(out,{},0,version);
    std::string tp;
    if(shortTeleport) {
        if(version>=6) {
            std::string e;integer(e,1,3);integer(e,2,102);integer(e,3,2);
            append(tp,uint32_t(e.size()));tp+=e;
        }else {append(tp,uint32_t(3));append(tp,uint32_t(102));tp.resize(172,0);}
    }
    auto ev=timer(version,100,0)+tp;
    for(const auto& event:positionEvents) ev+=event;
    ev+=timer(version,tickCount>7?97+tickCount:103,1);
    section(out,ev,2+shortTeleport+positionEvents.size(),version);return out;
}
template<class F> void rejects(F fn) {bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}assert(rejected);}
}
int main(int argc,char** argv) {
    if((argc==3||argc==4)&&std::string(argv[1])=="--sql") {
        line::Query query;query.map="kz_test";query.course="Main Course";query.pro=std::string(argv[2])=="pro";
        if(argc==4) query.rank=line::recordRankArgument(argv[3]).value();
        std::cout<<line::recordSQL(query,[](const std::string& s){return s;});return 0;
    }
    line::Query q;q.map="kz_test";q.course="Main Course";line::Record record;record.time=1;
    line::DecodeOptions raw;raw.lineError=0;
    for(unsigned version:{5u,7u}) {
        std::vector<Motion> attempts(19);
        const std::vector<line::Vec3> positions{
            {32,0,0},{48,0,0},{64,0,0},{68,0,0},{72,0,0},{76,0,0},{80,0,0},
            {180,0,0},{188,0,0},{204,0,0},{180,0,0},{64,4,0},{68,4,0},{72,4,0},{76,4,0},{80,4,0},{84,4,0}
        };
        for(size_t i=0;i<positions.size();++i) {
            auto& motion=attempts[i+2];
            motion.preOrigin=positions[i];
            motion.preVelocity=motion.postVelocity={256,0,0};
        }
        attempts[4].postGround=attempts[13].postGround=1;
        const auto first=positionEvent(version,107,{180,0,0});
        const auto replay=fixture(version,false,false,false,&attempts,{first,positionEvent(version,110,{180,0,0})});
        const auto filtered=line::decodeReplay(replay,q,record);
        assert(filtered.discardedAttemptFrames==9&&filtered.frames[2].serverTick==102&&filtered.frames[3].serverTick==112);
        assert(filtered.frames[2].section==filtered.frames[3].section);
        for(const auto& path:filtered.path) {
            assert(filtered.frames[path.lastFrame].serverTick<103||filtered.frames[path.firstFrame].serverTick>=112);
        }
        auto unfiltered=raw;unfiltered.hideFailedAttempts=false;
        const auto original=line::decodeReplay(replay,q,record,unfiltered);
        assert(original.frames.size()==17&&original.discardedAttemptFrames==0);
        attempts[12].preOrigin=line::Vec3{240,0,0};
        const auto forward=line::decodeReplay(fixture(version,false,false,false,&attempts,
            {first,positionEvent(version,110,{240,0,0})}),q,record);
        assert(forward.frames.size()==17&&forward.discardedAttemptFrames==0);
        assert(forward.checkpointSections.empty());
        if(version==7) {
            const auto returned=line::decodeReplay(fixture(version,false,false,false,&attempts,
                {first,positionEvent(version,110,{240,0,0},false,0)}),q,record);
            assert(returned.checkpointSections==std::vector<uint32_t>{returned.frames[10].section});
            attempts[12].preOrigin=line::Vec3{48,0,0};
            const auto checkpoint=line::decodeReplay(fixture(version,false,false,false,&attempts,
                {positionEvent(version,101,{48,0,0},true),first,positionEvent(version,110,{48,0,0},false,0)}),q,record);
            assert(checkpoint.discardedAttemptFrames==8&&checkpoint.frames[1].serverTick==101&&checkpoint.frames[2].serverTick==110);
        }
    }
    for(unsigned version=1;version<=7;++version) {
        const auto bytes=fixture(version);const auto route=line::decodeReplay(bytes,q,record,raw);
        assert(route.replayVersion==version);assert(route.segments.size()==3);
        assert(route.segments.front().start.x==32);assert(route.segments.back().end.x==80);
        const auto tp=line::decodeReplay(fixture(version,true),q,record);assert(!tp.segments.empty());
        for(auto s:tp.segments) assert(line::distance2(s.start,s.end)<=128*128);
        auto shortTP=line::decodeReplay(fixture(version,false,false,true),q,record);assert(shortTP.segments.size()==1);
        const auto simplified=line::decodeReplay(bytes,q,record);assert(simplified.segments.size()==1);
        assert(simplified.segments.front().start.x==32&&simplified.segments.front().end.x==80);
        std::vector<Motion> contacts(7);
        contacts[0].postGround=1; // Pre-run contact is not part of the route.
        contacts[3].postGround=1;contacts[3].preButtons=4;contacts[3].postDuck=2;contacts[3].postAmount=1;
        contacts[4].inherit=true;contacts[4].landedTick=777; // Preserve previous post ground/duck state.
        contacts[5].preGround=contacts[5].postGround=1;contacts[5].landedTick=777;
        auto annotated=line::decodeReplay(fixture(version,false,false,false,&contacts),q,record);
        assert(annotated.landings.size()==1&&annotated.landings[0].position.x==48&&annotated.landings[0].serverTick==101);
        assert(annotated.landings[0].phase==1&&annotated.landings[0].crouched&&annotated.landings[0].frameIndex==1);
        assert(annotated.frames.size()==4);
        assert(std::any_of(annotated.path.begin(),annotated.path.end(),[](const line::Segment& s){return s.start.x==48||s.end.x==48;}));
        std::vector<Motion> launches(7);launches[2].preGround=launches[2].postGround=launches[3].preGround=1;
        auto launched=line::decodeReplay(fixture(version,false,false,false,&launches),q,record);
        assert(std::any_of(launched.path.begin(),launched.path.end(),[](const line::Segment& s){return line::distance2(line::Vec3{48,0,0},s)<0.001f;})); // The displayed line still passes through takeoff.
        assert(std::any_of(annotated.segments.begin(),annotated.segments.end(),[](const line::Segment& s){return s.crouched;}));
        auto zero=raw;zero.maxLandings=0;rejects([&]{line::decodeReplay(fixture(version,false,false,false,&contacts),q,record,zero);});
        std::vector<Motion> air(7);for(auto& m:air) {m.preButtons=2;m.preVelocity.z=-100;m.postVelocity.z=280;m.landedTick=101;}
        assert(line::decodeReplay(fixture(version,false,false,false,&air),q,record).landings.empty()); // No guessed ModernJump positions.
        contacts[3].postGround=0;contacts[3].postDuck=2;contacts[4].inherit=false;
        contacts[4].preGround=contacts[4].postGround=1;
        auto boundary=line::decodeReplay(fixture(version,false,false,false,&contacts),q,record);
        assert(boundary.landings.size()==1&&boundary.landings[0].position.x==64&&boundary.landings[0].phase==0&&boundary.landings[0].crouched);
        contacts[3].postGround=1;contacts[4].preGround=0;contacts[4].postGround=1;
        auto consecutive=line::decodeReplay(fixture(version,false,false,false,&contacts),q,record);
        assert(consecutive.landings.size()==2); // Distinct consecutive contact frames are not merged.
        contacts[5].preGround=contacts[5].postGround=1;
        auto teleported=line::decodeReplay(fixture(version,false,false,true,&contacts),q,record);
        assert(teleported.landings.size()==2&&teleported.landings[0].section!=teleported.landings[1].section);
        for(const auto& contact:teleported.landings) assert(teleported.frames[contact.frameIndex].section==contact.section);
        const auto tpDrawing=line::buildDrawing(teleported,line::Settings{});
    assert(std::count_if(tpDrawing.begin(),tpDrawing.end(),[](const line::Primitive& p){return p.kind==line::PrimitiveKind::Landing;})==2);
        for(const auto& s:teleported.path) assert((s.start.x<=48&&s.end.x<=48)||(s.start.x>=96&&s.end.x>=96));
        contacts[4].preMoveType=contacts[4].postMoveType=7;
        rejects([&]{line::decodeReplay(fixture(version,false,false,false,&contacts),q,record);});
        for(auto& m:contacts) m.omitMoveType=true;
        assert(!line::decodeReplay(fixture(version,false,false,false,&contacts),q,record).landings.empty()); // Omitted move type defaults to WALK.
        std::vector<Motion> tap(7);tap[3].preButtons=4;
        auto exactLimit=raw;exactLimit.spacing=64;exactLimit.maxSegments=2;
        auto tapped=line::decodeReplay(fixture(version,false,false,false,&tap),q,record,exactLimit);assert(tapped.segments.size()==1);
        for(size_t n=0;n<bytes.size();++n) rejects([&]{line::decodeReplay(std::string_view(bytes.data(),n),q,record);});
        auto wrong=q;wrong.map="other";rejects([&]{line::decodeReplay(bytes,wrong,record);});
        wrong=q;wrong.course="main course";rejects([&]{line::decodeReplay(bytes,wrong,record);});
        wrong=q;wrong.mode="vanilla";rejects([&]{line::decodeReplay(bytes,wrong,record);});
        rejects([&]{line::decodeReplay(fixture(version,false,true),q,record);});
        line::DecodeOptions small;small.maxTicks=1;rejects([&]{line::decodeReplay(bytes,q,record,small);});
        small={};small.maxSegments=1;rejects([&]{line::decodeReplay(bytes,q,record,small);});
        small={};small.maxDecodedBytes=1;rejects([&]{line::decodeReplay(bytes,q,record,small);});
        small={};small.maxFileBytes=1;rejects([&]{line::decodeReplay(bytes,q,record,small);});
        auto corrupt=bytes;corrupt[corrupt.size()-5]^=0x5a;rejects([&]{line::decodeReplay(corrupt,q,record);});
    }
    std::vector<Motion> bounce(7);
    bounce[3].preHeight=2.5f;
    bounce[3].postOffset={5,0,-0.15625f};
    bounce[3].preVelocity={320,0,-320};
    bounce[3].postVelocity={320,0,292.625f};
    const auto recovered=line::decodeReplay(fixture(7,false,false,false,&bounce),q,record);
    assert(recovered.recoveredLandings==1&&recovered.landings.size()==1);
    const auto contact=recovered.landings.front().position;
    assert(std::abs(contact.x-50.5f)<0.001f&&std::abs(contact.z)<0.05f);
    assert(recovered.jumps.size()==1&&recovered.jumps[0].perfect&&line::distance2(recovered.jumps[0].position,contact)<0.0001f);
    std::vector<Motion> takeoff(7);
    takeoff[2].postGround=takeoff[3].preGround=1;
    takeoff[3].postVelocity.z=289.5f;
    takeoff[3].postOffset.z=4.6f;
    const auto quickJump=line::decodeReplay(fixture(7,false,false,false,&takeoff),q,record);
    assert(quickJump.jumps.size()==1&&quickJump.jumps[0].perfect);
    assert(quickJump.jumps[0].position.x==48&&quickJump.jumps[0].position.z==0);
    takeoff[3].postGround=takeoff[4].preGround=takeoff[4].postGround=takeoff[5].preGround=1;
    takeoff[5].postVelocity=takeoff[3].postVelocity;
    takeoff[5].postOffset=takeoff[3].postOffset;
    takeoff[3].postVelocity={};takeoff[3].postOffset={};
    const auto slowJump=line::decodeReplay(fixture(7,false,false,false,&takeoff),q,record);
    assert(slowJump.jumps.size()==1&&!slowJump.jumps[0].perfect);
    takeoff[5].postVelocity.z=-12.5f;
    assert(line::decodeReplay(fixture(7,false,false,false,&takeoff),q,record).jumps.empty());
    assert(std::any_of(recovered.path.begin(),recovered.path.end(),[&](const auto& s){return line::distance2(contact,s)<0.0001f;}));
    auto quickContact=bounce;
    quickContact[3].preVelocity.z=-951.125f;
    for(unsigned version:{5u,7u}) {
        const auto early=line::decodeReplay(fixture(version,false,false,false,&quickContact),q,record);
        assert(early.recoveredLandings==1&&early.landings.size()==1);
        const auto point=early.landings.front().position;
        assert(point.x>48.8f&&point.x<49.0f&&std::abs(point.z)<0.05f);
        assert(early.jumps.size()==1&&early.jumps[0].perfect&&std::abs(early.jumps[0].position.x-50.5f)<0.001f);
        assert(std::any_of(early.path.begin(),early.path.end(),[&](const auto& s){return line::distance2(point,s)<0.0001f;}));
    }
    quickContact[3].preHeight=0;
    quickContact[3].preVelocity.z=-4.25f;
    quickContact[3].postOffset.z=0.34375f;
    quickContact[3].postVelocity.z=295.75f;
    quickContact[3].postButtons=4; // The duck input changes while the hull remains standing.
    const auto shallow=line::decodeReplay(fixture(7,false,false,false,&quickContact),q,record);
    assert(shallow.recoveredLandings==1&&shallow.landings.size()==1&&shallow.landings.front().crouched);
    assert(line::decodeReplay(fixture(7,false,false,false,&quickContact,{positionEvent(7,101,{53,0,0})}),q,record).landings.empty());
    quickContact[3].postDuck=2;
    assert(line::decodeReplay(fixture(7,false,false,false,&quickContact),q,record).landings.empty());
    quickContact[3].postDuck=0;
    quickContact[3].postOffset.z=6;
    assert(line::decodeReplay(fixture(7,false,false,false,&quickContact),q,record).landings.empty());
    bounce[3].postVelocity={0,320,292.625f}; // Large lateral deflection is ambiguous.
    assert(line::decodeReplay(fixture(7,false,false,false,&bounce),q,record).landings.empty());
    bounce[3].postGround=1;
    bounce[3].postOffset={5,0,-2.5f};
    bounce[3].postVelocity={320,0,0};
    const auto corrected=line::decodeReplay(fixture(7,false,false,false,&bounce),q,record);
    assert(corrected.recoveredLandings==0&&corrected.landings.size()==1);
    assert(corrected.landings[0].position.x>50&&corrected.landings[0].position.x<51&&corrected.landings[0].position.z==0);
    std::vector<Motion> edge(7);
    edge[3].preHeight=0.666015625f;
    edge[3].preVelocity={300,0,17.625f};
    edge[3].postOffset={4.5f,0,1.7001953125f};
    edge[3].postVelocity={280,0,295.75f};
    edge[3].preDuck=edge[3].postDuck=6;
    edge[5].postGround=1;
    auto supportedEdge=line::decodeReplay(fixture(7,false,false,false,&edge),q,record);
    assert(supportedEdge.landings.size()==1&&supportedEdge.landingCandidates.size()==1);
    auto emptyEdge=supportedEdge;
    emptyEdge.confirmLandings([](line::Vec3){return std::optional<float>{};});
    assert(emptyEdge.landings.size()==1&&emptyEdge.recoveredLandings==0&&emptyEdge.landingCandidates.empty());
    assert(emptyEdge.jumps.empty());
    supportedEdge.confirmLandings([](line::Vec3 p)->std::optional<float>{
        assert(p.x==50.25f&&std::abs(p.z-0.03125f)<0.001f);
        return 0.0f;
    });
    assert(supportedEdge.recoveredLandings==1&&supportedEdge.landings.size()==2&&supportedEdge.landingCandidates.empty());
    assert(supportedEdge.landings[0].serverTick==101&&supportedEdge.landings[0].position.z==0&&supportedEdge.landings[0].crouched);
    assert(supportedEdge.landings[1].serverTick==103);
    assert(supportedEdge.jumps.size()==1&&!supportedEdge.jumps[0].perfect&&supportedEdge.jumps[0].position.z==0);
    edge[3].preHeight=11.87890625f;
    edge[3].preVelocity.z=-119.875f;
    edge[3].postOffset.z=-9.537109375f;
    edge[3].postVelocity.z=292.625f;
    edge[3].postDuck=0;
    const auto uncrouched=line::decodeReplay(fixture(7,false,false,false,&edge),q,record);
    assert(uncrouched.landingCandidates.size()==1&&uncrouched.landingCandidates[0].crouched);
    assert(line::decodeReplay(fixture(7,false,false,false,&edge,{positionEvent(7,101,{52,0,2})}),q,record).landingCandidates.empty());
    std::vector<Motion> ladder(7);
    ladder[3].postMoveType=9;
    ladder[3].postOffset={2,0,4};
    ladder[3].postButtons=4;
    ladder[4].preMoveType=ladder[4].postMoveType=9;
    ladder[5].preMoveType=9;
    for(unsigned version:{1u,5u,7u}) {
        const auto route=line::decodeReplay(fixture(version,false,false,false,&ladder),q,record);
        assert(route.landings.size()==1&&route.recoveredLandings==0);
        const auto& point=route.landings.front();
        assert(point.ladder&&point.crouched&&point.serverTick==101&&point.phase==1);
        assert(point.position.x==50&&point.position.z==4);
        assert(std::any_of(route.path.begin(),route.path.end(),[&](const auto& s){return line::distance2(point.position,s)<0.0001f;}));
    }
    ladder[3]=Motion{};
    const auto boundaryLadder=line::decodeReplay(fixture(7,false,false,false,&ladder),q,record);
    assert(boundaryLadder.landings.size()==1&&boundaryLadder.landings.front().ladder&&boundaryLadder.landings.front().phase==0);
    assert(line::decodeReplay(fixture(7,false,false,true,&ladder),q,record).landings.empty());
    rejects([&]{line::decodeReplay(fixture(8),q,record);});
    auto wrongTime=record;wrongTime.time=2;rejects([&]{line::decodeReplay(fixture(7),q,wrongTime);});
    std::atomic<bool> cancel{true};rejects([&]{line::decodeReplay(fixture(7),q,record,{},&cancel);});
    line::Route route;route.segments={{{-100,0,0},{100,0,0}},{{0,50,0},{0,60,0},true},{{10000,0,0},{10100,0,0}}};
    assert(line::uuid("019aadc6-b058-7892-a53b-d13c39b94b8a"));assert(!line::uuid("../../not-a-replay"));
    const auto url=line::recordsURL("https://api.cs2kz.org",q);assert(url.find("course=Main%20Course")!=std::string::npos);assert(url.find("has_teleports=false")!=std::string::npos);
    auto ranked=q;ranked.rank=line::recordRankArgument("2").value();
    assert(line::recordsURL("https://api.cs2kz.org",ranked).ends_with("&limit=1&offset=1"));
    assert(line::recordRankArgument("1000")==line::MaxRecordRank&&!line::recordRankArgument("bonus2"));
    for(const char* invalid:{"0","-1","1001","9999999999999"}) rejects([&]{line::recordRankArgument(invalid);});
    auto sql=line::recordSQL(q,[](std::string s){return s;});assert(sql.find("Teleports=0")!=std::string::npos);assert(sql.find("StyleIDFlags=0")!=std::string::npos);assert(sql.find("NOT EXISTS")!=std::string::npos);
    q.pro=false;sql=line::recordSQL(q,[](std::string s){return s;});assert(sql.find("Teleports=0")==std::string::npos);
    assert(argc==2);
    const auto flatSettings=line::readSettings(argv[1],{});
    assert(flatSettings.global&&flatSettings.pro&&flatSettings.profile=="workshop-flat"&&flatSettings.useFlatMarkers());
    rejects([&]{line::readSettings(argv[1],"missing");});
    line::Json config;{std::ifstream f(argv[1]);f>>config;}
    assert(config.at("profiles").size()==1);
    const std::string temp="routeline-test-config.json";
    auto writeConfig=[&] {std::ofstream f(temp);f<<config;};
    config.erase("active_profile");
    writeConfig();
    assert(line::readSettings(temp,{}).profile=="workshop-flat");
    // Standalone fixtures exercise legacy/custom configs without shipping extra profiles.
    config["profiles"]["cs2kz"]={{"render",line::Json::object()}};
    config["profiles"]["local-test"]={{"source","sr"}};
    config["active_profile"]="cs2kz";
    auto& render=config["profiles"]["cs2kz"]["render"];
    render["landing_size"]=20;
    render.erase("mode");render["radius"]=1000;render["segments_per_player"]=64;render["max_total_elements"]=1024;
    constexpr uint64_t reloadAdmin=76561198000000000ULL;
    config.erase("reload_admins");
    render.erase("path_batch_frames");
    writeConfig();
    const auto legacyConfig=line::readSettings(temp,{});
    assert(legacyConfig.pathBatchFrames==144&&!legacyConfig.canReloadFromChat(reloadAdmin));
    config["reload_admins"]={std::to_string(reloadAdmin)};
    render["path_batch_frames"]=96;
    writeConfig();
    const auto tuned=line::readSettings(temp,{});
    assert(tuned.profile=="cs2kz"&&tuned.pathBatchFrames==96&&tuned.canReloadFromChat(reloadAdmin));
    assert(!tuned.canReloadFromChat(0)&&!tuned.canReloadFromChat(reloadAdmin+1));
    const auto switched=line::readSettings(temp,"local-test");
    assert(switched.profile=="local-test"&&!switched.global&&switched.canReloadFromChat(reloadAdmin));
    render["markers_only"]=true;writeConfig();
    assert((line::readSettings(temp,{}).layers==line::DisplayLayers{false,false,true}));
    render["landing_markers"]=false;writeConfig();
    rejects([&]{line::readSettings(temp,{});});
    render.erase("landing_markers");render.erase("markers_only");
    config["reload_admins"]={std::to_string(reloadAdmin)+"junk"};
    writeConfig();rejects([&]{line::readSettings(temp,{});});
    config["reload_admins"]=line::Json::array();
    for(auto invalid:{line::Json(-1),line::Json(257),line::Json(96.5)}) {
        render["path_batch_frames"]=invalid;
        writeConfig();rejects([&]{line::readSettings(temp,{});});
    }
    render.erase("path_batch_frames");
    render["route_particle_spacing"]=100;
    render.erase("marker_particle_spacing");
    writeConfig();
    const auto wideSpacing=line::readSettings(temp,{});
    assert(wideSpacing.routeParticleSpacing==100&&wideSpacing.markerParticleSpacing==8);
    render["route_particle_spacing"]=101;
    writeConfig();rejects([&]{line::readSettings(temp,{});});
    render.erase("route_particle_spacing");
    render["route_distance"]=640;
    writeConfig();assert(line::readSettings(temp,{}).routeDistance==640&&line::readSettings(temp,{}).flatMarkerDistance()==600);
    render["marker_distance"]=0;
    writeConfig();assert(line::readSettings(temp,{}).flatMarkerDistance()==0);
    render["marker_distance"]=8193;
    writeConfig();rejects([&]{line::readSettings(temp,{});});
    render.erase("marker_distance");
    render["route_distance"]=-1;
    writeConfig();rejects([&]{line::readSettings(temp,{});});
    render.erase("route_distance");
    writeConfig();assert(line::readSettings(temp,{}).totalElements==4096);
    render["mode"]="persistent";writeConfig();assert(line::readSettings(temp,{}).totalElements==1024);
    render["mode"]="follow";writeConfig();rejects([&]{line::readSettings(temp,{});});
    render["mode"]="persistent";render["renderer"]="worldtext";render["z_offset"]=1.5;render["jump_size"]=32;render["band_width"]=2;
    render["color"]={0,0,255,255};render["route_color"]={255,255,255,255};writeConfig();
    const auto migrated=line::readSettings(temp,{});
    assert(migrated.width==0.5f&&migrated.landingSize==20&&migrated.zOffset==1.5f&&migrated.replayFrames==3000&&migrated.backend==line::RenderBackend::Workshop);
    assert(migrated.colors[0]==255&&migrated.colors[1]==255&&migrated.colors[2]==255);
    render["replay_frames"]=3001;writeConfig();rejects([&]{line::readSettings(temp,{});});
    render["replay_frames"]=2500;render.erase("path_style");render["path_xy_error"]=6;render["path_z_error"]=1;render["max_path_length"]=128;writeConfig();
    const auto oldPath=line::readSettings(temp,{});assert(oldPath.decode.smoothPath&&oldPath.decode.pathZError==3&&oldPath.decode.maxPathLength==256);
    render["path_style"]="precise";writeConfig();const auto preserved=line::readSettings(temp,{});
    assert(!preserved.decode.smoothPath&&preserved.decode.pathZError==1&&preserved.decode.maxPathLength==128);
    render["renderer"]="workshop";render.erase("replay_frames");render.erase("window_overlap");render.erase("follow");writeConfig();
    const auto workshopDefaults=line::readSettings(temp,{});
    assert(workshopDefaults.follow&&workshopDefaults.replayFrames==3000&&workshopDefaults.windowOverlap==1500);
    // Retired sizing cannot reintroduce terrain-dependent or oversized frames.
    assert(workshopDefaults.landingSize==20&&workshopDefaults.landingWidth==0.85f);
    render["landing_size"]=32;render["renderer"]="model";writeConfig();
    const auto retiredModel=line::readSettings(temp,{});
    assert(retiredModel.backend==line::RenderBackend::Workshop&&retiredModel.landingSize==20);
    render["renderer"]="workshop";render["landing_width"]=0.1f;writeConfig();rejects([&]{line::readSettings(temp,{});});
    render.erase("landing_width");
    render["replay_frames"]=800;writeConfig();
    assert(line::readSettings(temp,{}).windowOverlap==400);
    render["window_overlap"]=0;writeConfig();
    assert(line::readSettings(temp,{}).windowOverlap==0);
    render["window_overlap"]=800;writeConfig();rejects([&]{line::readSettings(temp,{});});
    std::remove(temp.c_str());
    auto settings=legacyConfig;
    settings.backend=line::RenderBackend::Beam;settings.windowOverlap=0;settings.landingSize=20;settings.colors[4]=40;
    route.landings={{{0,0,0},10,false,0,0},{{10000,0,0},20,false,0,0}};
    route.path={{{0,0,0},{5000,0,0},false},{{5000,0,0},{10000,0,0},true}};
    const auto drawing=line::buildLegacyDrawing(route,settings);assert(drawing.size()==10);
    assert(drawing[0].kind==line::PrimitiveKind::Route&&drawing[0].segment.end.x==5000);
    assert(drawing[1].kind==line::PrimitiveKind::Crouch&&drawing[1].segment.start.x==5000&&drawing[1].segment.end.x==10000);
    const auto outline=line::landingOutline({10,20,30},20);
    for(size_t i=0;i<4;++i) {
        assert(line::distance2(outline[i].start,outline[i].end)==400);
        assert(line::distance2(outline[i].end,outline[(i+1)%4].start)==0);
        assert(outline[i].start.z==30);
    }
    auto detail=settings;
    detail.detail=line::DisplayDetail::Compact;
    const auto compact=line::buildLegacyDrawing(route,detail);
    assert(compact.size()==4&&compact[2].kind==line::PrimitiveKind::Landing);
    assert(line::distance2(compact[2].segment.start,compact[2].segment.end)==64);
    detail.detail=line::DisplayDetail::Landings;
    assert(line::buildLegacyDrawing(route,detail).size()==2);
    const auto defaultFollow=legacyConfig;
    assert(defaultFollow.backend==line::RenderBackend::Workshop&&defaultFollow.follow&&defaultFollow.replayFrames==3000&&defaultFollow.windowOverlap==1500);
    assert(defaultFollow.routeParticleSpacing==20&&defaultFollow.markerParticleSpacing==8);
    auto packedSettings=flatSettings;
    packedSettings.flatMarkers=false;
    assert(packedSettings.packedRoute&&packedSettings.pathBatchFrames==320&&packedSettings.pathBatchLength==1600);
    assert(flatSettings.useFlatMarkers()&&flatSettings.packedRoute&&!packedSettings.useFlatMarkers());
    auto workshop=defaultFollow;
    workshop.replayFrames=1000;
    workshop.windowOverlap=200;
    // Batching preserves every route vertex, frame span and color boundary.
    line::Route longPath;
    for(uint32_t i=0;i<16;++i) longPath.path.push_back({{float(i*80),0,0},{float((i+1)*80),0,0},false,i*8,(i+1)*8,0});
    auto previous=workshop;previous.pathBatchLength=640;
    assert(line::buildDrawing(longPath,previous).size()==2);
    const auto batched=line::buildDrawing(longPath,workshop);
    assert(batched.size()==1&&batched[0].ropeNodes.size()==17&&batched[0].lastFrame==128);
    auto shortBatches=workshop;
    shortBatches.pathBatchFrames=tuned.pathBatchFrames;
    assert(line::buildDrawing(longPath,shortBatches).size()==2);
    for(size_t i=0;i<longPath.path.size();++i) {
        assert(line::distance2(batched[0].ropeNodes[i],longPath.path[i].start)==0);
        assert(line::distance2(batched[0].ropeNodes[i+1],longPath.path[i].end)==0);
    }
    longPath.path[8].crouched=true;
    assert(line::buildDrawing(longPath,workshop).size()==3);
    line::Route ropeRoute;
    ropeRoute.path={{{0,0,0},{8,0,0},false,0,2,0},{{8,0,0},{12,4,0},false,2,4,0},
        {{12,4,0},{16,4,0},true,4,6,0},{{16,4,0},{24,4,0},true,6,8,1}};
    ropeRoute.landings={{{12,4,0},4,false,0,0,4}};
    const auto ropes=line::buildDrawing(ropeRoute,workshop);
    assert(ropes.size()==4&&ropes[0].ropeNodes.size()==3&&ropes[0].lastFrame==4);
    assert(ropes[1].kind==line::PrimitiveKind::Crouch&&ropes[2].segment.section==1);
    assert(ropes[3].kind==line::PrimitiveKind::Landing);
    const auto packedRopes=line::buildDrawing(ropeRoute,packedSettings);
    assert(packedRopes.size()==3&&packedRopes[0].ropeNodes.size()==5);
    assert(packedRopes[0].nodeCrouched==std::vector<bool>({false,false,false,true,true}));
    assert(line::distance2(packedRopes[0].ropeNodes[3],ropeRoute.path[2].start)==0);
    assert(line::distance2(packedRopes[0].ropeNodes[2],ropeRoute.path[1])<0.0001f);
    assert(line::distance2(packedRopes[0].ropeNodes[2],packedRopes[0].ropeNodes[3])<=0.25f*0.25f+0.00001f);
    assert(packedRopes[1].segment.section==1&&packedRopes[1].nodeCrouched.empty());
    assert(packedRopes[2].kind==line::PrimitiveKind::Landing&&packedRopes[2].firstFrame==4);
    auto differentAlpha=packedSettings;
    differentAlpha.colors[11]=100;
    assert(line::buildDrawing(ropeRoute,differentAlpha).size()==ropes.size());
    assert(line::workshopNodeColors({false,true},packedSettings)==
        "[[1.000000,1.000000,1.000000],[1.000000,0.156863,0.156863]]");
    assert(line::selectDrawing(ropes,{0,5,false})==std::vector<size_t>({0,3}));
    line::PersistentDisplays<int> workshopDisplays;
    const auto workshopRoute=std::make_shared<line::Route>(ropeRoute);
    workshopDisplays.attach(0,workshopRoute,{},workshop,[](int){});
    workshopDisplays.attach(63,workshopRoute,{},workshop,[](int){});
    assert(workshopDisplays.group(0)!=workshopDisplays.group(63)&&workshopDisplays.reserved()==8);
    workshopDisplays.clear(0,[](int){});
    assert(workshopDisplays.reserved()==4&&workshopDisplays.group(63));
    assert(line::workshopPathNodes({{50,60,70},{56,60,70}})==
        "[[0.00000,0.00000,0.00000,0.00000,0.00000,0.00000,2.00000,0.00000,0.00000],"
        "[6.00000,0.00000,0.00000,-2.00000,0.00000,0.00000,0.00000,0.00000,0.00000]]");
    const auto noGround=[](line::Vec3) -> std::optional<line::GroundHit> {return std::nullopt;};
    // Unsupported contacts still keep a full square in midair.
    for(const auto origin:std::vector<line::Vec3>{{0,0,0},{4,32,1},{200,-300,1000}}) {
        const auto marker=line::workshopMarker(origin,workshop,noGround);
        assert(marker.size()==5&&line::distance2(marker.front(),marker.back())==0);
        for(size_t i=0;i<4;++i) {
            assert(line::distance2(marker[i],marker[i+1])==400);
            assert(marker[i].z==origin.z+workshop.zOffset);
        }
        assert(line::distance2((marker[0]+marker[2])*0.5f,origin+line::Vec3{0,0,workshop.zOffset})<0.0001f);
    }
    auto oldSize=workshop;oldSize.landingSize=128;
    const auto fixedMarker=line::workshopMarker({},oldSize,noGround);
    assert(line::distance2(fixedMarker[0],fixedMarker[1])==400);
    oldSize.detail=line::DisplayDetail::Compact;
    assert(line::workshopMarker({},oldSize,noGround).size()==2); // Explicit compact mode stays a short mark.
    const auto edgeGround=[](line::Vec3 p) -> std::optional<line::GroundHit> {
        if(p.x<6||p.x>26||std::abs(p.y)>3||std::abs(p.z)>6) return std::nullopt;
        return line::GroundHit{{p.x,p.y,0},{0,0,1}};
    };
    const auto correctedMarker=line::workshopMarker({0,0,1},workshop,edgeGround);
    const auto ladderCenter=line::workshopMarkerCenter({0,0,1},workshop,edgeGround,true);
    assert(ladderCenter.x==0&&ladderCenter.y==0&&ladderCenter.z==1+workshop.zOffset);
    assert(correctedMarker.size()==5);
    assert(line::distance2((correctedMarker[0]+correctedMarker[2])*0.5f,line::Vec3{8,0,2.5f})==0);
    for(size_t i=0;i<4;++i) assert(line::distance2(correctedMarker[i],correctedMarker[i+1])==400);
    assert(!edgeGround(correctedMarker[0])); // Unsupported corners no longer shrink the frame.
    line::PersistentDisplays<int> displays;size_t erased=0;auto erase=[&](int){++erased;};
    auto shared=std::make_shared<line::Route>(route);settings.totalElements=4;
    displays.attach(0,shared,{},settings,erase);displays.groups()[0]->entries[0].visual=10;
    const auto* original=displays.group(0);
    displays.attach(63,shared,{},settings,erase);assert(displays.group(0)==displays.group(63));
    assert(displays.reserved()==4&&displays.group(63)->entries[0].visual==10);
    auto another=std::make_shared<line::Route>(route);
    rejects([&]{displays.attach(0,another,{},settings,erase);});
    assert(displays.group(0)==original&&erased==0);
    displays.clear(0,erase);assert(displays.group(63)==original&&erased==0);
    displays.attach(63,another,{},settings,erase);assert(erased==1&&displays.reserved()==4);
    displays.clear(63,erase);assert(displays.groups().empty()&&displays.reserved()==0);
    settings.totalElements=3;rejects([&]{displays.attach(0,shared,{},settings,erase);});assert(!displays.group(0));
    settings.landingMarkers=false;assert(line::buildLegacyDrawing(route,settings).size()==2);settings.landingMarkers=true;
    auto markersOnly=route;markersOnly.path.clear();assert(line::buildLegacyDrawing(markersOnly,settings).size()==8); // No inferred straight connector.
    assert(line::primitiveColor(line::PrimitiveKind::Route,settings)[0]==255&&line::primitiveColor(line::PrimitiveKind::Route,settings)[2]==255);
    assert(line::primitiveColor(line::PrimitiveKind::Landing,settings)[0]==40&&line::primitiveColor(line::PrimitiveKind::Landing,settings)[2]==255);
    assert(line::primitiveColor(line::PrimitiveKind::Crouch,settings)[0]==255&&line::primitiveColor(line::PrimitiveKind::Crouch,settings)[1]==40);
    line::Route indexed;
    for(uint32_t i=0;i<6000;++i) indexed.frames.push_back({{float(i*8),float(i%7),0},i});
    auto linear=indexed;indexed.indexFrames();
    for(line::Vec3 p:std::vector<line::Vec3>{{-100,10,0},{8,0,0},{39000,12,5},{999999,0,0}}) assert(indexed.nearestFrame(p)==linear.nearestFrame(p));
    assert((line::drawingWindow(indexed,indexed.frames[1499].position,defaultFollow)==line::FrameWindow{0,3000,false}));
    const auto handoff=line::drawingWindow(indexed,indexed.frames[1500].position,defaultFollow);
    assert((handoff==line::FrameWindow{1500,4500,false}));
    assert(line::drawingWindow(indexed,indexed.frames[1499].position,defaultFollow,handoff)==handoff);
    assert((line::drawingWindow(indexed,indexed.frames[3000].position,defaultFollow)==line::FrameWindow{3000,6000,false}));
    indexed.path={{{0,0,0},{8,0,0},false,1475,1499,0},{{8,0,0},{16,0,0},false,1499,1505,0},
        {{16,0,0},{24,0,0},true,1505,1510,0}};
    const auto boundaryPaths=line::buildDrawing(indexed,packedSettings);
    assert(boundaryPaths.size()==3&&boundaryPaths.front().lastFrame==1499);
    auto marks=flatSettings;
    marks.detail=line::DisplayDetail::Full;
    line::Route flatRoute;
    flatRoute.frames=indexed.frames;
    flatRoute.landings={{{0,0,0},1,false,0,0,1},{{0,1,0},2,true,0,0,2},{{0,2,0},3,false,0,0,3},
        {{0,3,0},1499,true,0,0,1499},{{0,4,0},1500,false,0,0,1500},{{0,5,0},1501,true,0,0,1501}};
    const auto flat=line::buildDrawing(flatRoute,marks);
    assert(flat.size()==4&&flat[0].flatMarkerOrigins.size()==2&&flat[1].flatMarkerOrigins.size()==2);
    assert(!flat[0].segment.crouched&&flat[1].segment.crouched&&flat[1].lastFrame==1499);
    assert(line::primitiveColor(flat[0],marks)==marks.colors.data()+4&&line::primitiveColor(flat[1],marks)==marks.colors.data()+8);
    assert(&line::workshopFlatMarkerAsset(line::primitiveColor(flat[0],marks))==&line::WorkshopFlatMarkerBlue);
    assert(&line::workshopFlatMarkerAsset(line::primitiveColor(flat[1],marks))==&line::WorkshopFlatMarkerRed);
    auto customMarks=marks;
    customMarks.colors[7]=128;
    assert(&line::workshopFlatMarkerAsset(line::primitiveColor(flat[0],customMarks))==&line::WorkshopFlatMarkerBlue);
    customMarks.colors[4]=80;
    assert(&line::workshopFlatMarkerAsset(line::primitiveColor(flat[0],customMarks))==&line::WorkshopFlatMarkerCustom);
    assert(line::selectDrawing(flat,handoff)==std::vector<size_t>({2,3}));
    auto meshMarks=marks;meshMarks.meshMarkers=true;
    auto meshRoute=flatRoute;
    meshRoute.jumps={{{0,0,0},1,0,false},{{2,0,0},2,0,false}};
    const auto meshDrawing=line::buildDrawing(meshRoute,meshMarks);
    assert(meshDrawing.size()==meshRoute.landings.size()+meshRoute.jumps.size());
    assert(std::all_of(meshDrawing.begin(),meshDrawing.end(),[](const auto& p){return p.flatMarkerOrigins.size()==1;}));
    auto mixedContacts=flatRoute;
    mixedContacts.landings[2].ladder=true;
    const auto mixedDrawing=line::buildDrawing(mixedContacts,marks);
    assert(mixedDrawing.size()==flat.size()+1);
    const auto ladderGroup=std::find_if(mixedDrawing.begin(),mixedDrawing.end(),[](const auto& p){return p.ladderContact;});
    assert(ladderGroup!=mixedDrawing.end()&&ladderGroup->flatMarkerOrigins.size()==1&&ladderGroup->firstFrame==3);
    auto distantMarks=std::make_shared<line::Route>();
    distantMarks->frames=flatRoute.frames;
    distantMarks->landings={{{0,0,0},1,false,0,0,1},{{240,0,0},2,false,0,0,2},
        {{500,0,0},3,false,0,0,3},{{520,0,0},4,false,0,0,4}};
    const auto spatialBatches=line::buildDrawing(*distantMarks,marks);
    assert(spatialBatches.size()==2&&spatialBatches[0].flatMarkerOrigins.size()==2&&spatialBatches[1].flatMarkerOrigins.size()==2);
    auto nearMarks=marks;nearMarks.routeDistance=128;nearMarks.markerDistance.reset();nearMarks.zOffset=0;
    line::PersistentDisplays<int> markerDisplay;
    auto showMarkersAt=[&](float x) {markerDisplay.attach(0,distantMarks,{x,0,0},nearMarks,[](int){});};
    showMarkersAt(240);
    assert(markerDisplay.selected(0)==std::vector<size_t>{0}); // Second contact is close even though the origin is far.
    const auto markerWindow=markerDisplay.window(0);
    showMarkersAt(520);
    assert(markerDisplay.window(0)==markerWindow&&markerDisplay.selected(0)==std::vector<size_t>{1});
    nearMarks.follow=false;
    showMarkersAt(520);
    assert(markerDisplay.selected(0)==std::vector<size_t>({0,1}));
    auto splitDistances=std::make_shared<line::Route>(*distantMarks);
    splitDistances->path={{{500,0,0},{520,0,0},false,3,4,0}};
    splitDistances->jumps={{{240,0,0},2,0,false},{{520,0,0},4,0,false}};
    nearMarks.follow=true;
    nearMarks.routeDistance=700;
    nearMarks.markerDistance=128;
    markerDisplay.attach(0,splitDistances,{240,0,0},nearMarks,[](int){});
    assert(markerDisplay.selected(0)==std::vector<size_t>({0,1,3}));
    nearMarks.markerDistance=0;
    markerDisplay.attach(0,splitDistances,{240,0,0},nearMarks,[](int){});
    assert(markerDisplay.selected(0)==std::vector<size_t>({0,1,2,3,4}));
    marks.windowOverlap=0;
    assert(line::buildDrawing(flatRoute,marks).size()==flatRoute.landings.size());
    settings.follow=true;
    settings.replayFrames=2500;auto window=line::drawingWindow(indexed,indexed.frames[4000].position,settings);
    assert(window.begin==2750&&window.end==5250&&window.end-window.begin==2500);
    window=line::drawingWindow(indexed,indexed.frames[2].position,settings);assert(window.begin==0&&window.end==2500);
    window=line::drawingWindow(indexed,indexed.frames.back().position,settings);assert(window.begin==3500&&window.end==6000);
    assert((line::drawingWindow(indexed,indexed.frames[799].position,workshop)==line::FrameWindow{0,1000,false}));
    assert((line::drawingWindow(indexed,indexed.frames[800].position,workshop)==line::FrameWindow{800,1800,false}));
    assert((line::drawingWindow(indexed,indexed.frames[1600].position,workshop)==line::FrameWindow{1600,2600,false}));
    assert((line::drawingWindow(indexed,indexed.frames.back().position,workshop)==line::FrameWindow{5000,6000,false}));
    line::Route shortRoute;
    shortRoute.frames.assign(indexed.frames.begin(),indexed.frames.begin()+300);
    assert((line::drawingWindow(shortRoute,shortRoute.frames.back().position,workshop)==line::FrameWindow{0,300,false}));
    // A 1000-frame budget must not combine separate passes around a teleport.
    auto separated=indexed;
    for(size_t i=0;i<separated.frames.size();++i) separated.frames[i].section=i<2200?0:i<2326?1:2;
    const auto isolated=line::drawingWindow(separated,separated.frames[2255].position,workshop);
    assert((isolated==line::FrameWindow{2200,2326,false}));
    assert((line::drawingWindow(separated,separated.frames[2328].position,workshop,isolated)==line::FrameWindow{2326,3326,false}));
    assert((line::drawingWindow(separated,separated.frames[2199].position,workshop,isolated)==line::FrameWindow{1200,2200,false}));
    auto combined=workshop;combined.separateSections=false;
    assert((line::drawingWindow(separated,separated.frames[2255].position,combined)==line::FrameWindow{1600,2600,false}));

    // A Workshop page retains its overlap even if the player briefly moves back across the handoff.
    auto paged=std::make_shared<line::Route>();paged->frames=indexed.frames;paged->indexFrames();
    for(uint32_t i:{799,800,980,1000,1799,1800}) paged->landings.push_back({indexed.frames[i].position,i,false,0,0,i});
    std::vector<int> stopped;
    auto pageAt=[&](size_t frame) {
        workshopDisplays.attach(0,paged,indexed.frames[frame].position,workshop,[&](int visual){stopped.push_back(visual);});
        return workshopDisplays.window(0);
    };
    assert((pageAt(799)==line::FrameWindow{0,1000,false}));
    auto& entries=workshopDisplays.groups().back()->entries;
    entries[0].visual=10;entries[1].visual=11;
    assert((pageAt(800)==line::FrameWindow{800,1800,false}));
    assert(workshopDisplays.selected(0)==std::vector<size_t>({1,2,3,4})&&entries[1].visual==11&&stopped==std::vector<int>{10});
    assert((pageAt(799)==line::FrameWindow{800,1800,false}));
    assert((pageAt(767)==line::FrameWindow{0,1000,false}));
    assert((pageAt(5800)==line::FrameWindow{5000,6000,false}));
    assert(stopped==std::vector<int>({10,11}));
    // Following remains active through repeated handoffs and returning to the start.
    for(size_t frame=0;frame<6000;frame+=80) {
        const auto next=pageAt(frame);
        assert(next.begin<=frame&&frame<next.end&&next.end-next.begin==1000);
    }
    assert((pageAt(0)==line::FrameWindow{0,1000,false}));
    auto fullRoute=workshop;
    fullRoute.follow=false;
    fullRoute.totalElements=5;
    entries[0].visual=12;
    auto showFull=[&](size_t frame) {
        workshopDisplays.attach(0,paged,indexed.frames[frame].position,fullRoute,[&](int visual){stopped.push_back(visual);});
    };
    rejects([&]{showFull(0);});
    assert((workshopDisplays.window(0)==line::FrameWindow{0,1000,false})&&entries[0].visual==12);
    fullRoute.totalElements=workshop.totalElements;
    showFull(0);
    showFull(5999);
    assert((workshopDisplays.window(0)==line::FrameWindow{0,6000,true}));
    assert(workshopDisplays.selected(0).size()==6&&entries[0].visual==12&&stopped==std::vector<int>({10,11}));
    assert((pageAt(0)==line::FrameWindow{0,1000,false})&&entries[0].visual==12);
    // Distance changes can update paths within the same window while keeping the blue markers.
    auto localRoute=std::make_shared<line::Route>();
    localRoute->frames.resize(16);
    localRoute->path={{{0,0,0},{10,0,0},false,0,4,0},{{250,0,0},{260,0,0},true,8,12,0}};
    localRoute->landings={{{1000,0,0},10,false,0,0,10}};
    auto nearby=defaultFollow;
    nearby.routeDistance=128;
    nearby.zOffset=0;
    line::PersistentDisplays<int> localDisplay;
    std::vector<int> localStops;
    auto lookAt=[&](float x) {localDisplay.attach(0,localRoute,{x,0,0},nearby,[&](int id){localStops.push_back(id);});};
    lookAt(0);
    const auto localWindow=localDisplay.window(0);
    assert(localDisplay.selected(0)==std::vector<size_t>({0,2}));
    auto& localEntries=localDisplay.groups().front()->entries;
    localEntries[0].visual=13;
    localEntries[2].visual=14;
    lookAt(144);
    assert(localDisplay.window(0)==localWindow&&localDisplay.selected(0)==std::vector<size_t>({0,1,2}));
    lookAt(190);
    assert(localDisplay.window(0)==localWindow&&localDisplay.selected(0)==std::vector<size_t>({1,2}));
    assert(localStops==std::vector<int>({13})&&localEntries[2].visual==14);
    nearby.follow=false;
    lookAt(190);
    assert(localDisplay.window(0).all&&localDisplay.selected(0)==std::vector<size_t>({0,1,2}));
    // The next batch on a steep descent is visible before entering its usual radius.
    auto descent=std::make_shared<line::Route>();
    for(uint32_t i=0;i<=48;++i) {
        const auto steep=std::min(i,16u);
        descent->frames.push_back({{0,0,-float(steep)*9-float(i-steep)*0.375f},i,0});
    }
    descent->path={{{0,0,0},{0,0,-144},false,0,16,0},
        {{0,0,-144},{0,0,-150},false,16,32,0},{{0,0,-150},{0,0,-156},false,32,48,0}};
    descent->landings={{{0,0,-145},18,false,0,0,18},{{0,0,-149},30,true,0,0,30}};
    auto previewSettings=nearby;
    previewSettings.follow=true;
    previewSettings.flatMarkers=true;
    previewSettings.pathBatchFrames=16;
    line::PersistentDisplays<int> previewDisplay;
    previewSettings.totalElements=1;
    previewDisplay.attach(0,descent,{0,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>{0}); // Keep the regular selection if the budget is full.
    previewDisplay.clear(0,[](int){});
    previewSettings.totalElements=defaultFollow.totalElements;
    previewDisplay.attach(0,descent,{0,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>({0,1,3}));
    assert((previewDisplay.window(0)==line::FrameWindow{0,49,false}));
    previewDisplay.attach(0,descent,{34,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>({0,1,3}));

    // Preview the next TP section as its own effect while keeping the 3000-frame limit.
    auto seam=std::make_shared<line::Route>();
    for(uint32_t i=0;i<3200;++i) seam->frames.push_back(
        {{float(i),0,i<3000?0.0f:320.0f},i,uint32_t(i>=3000)});
    seam->path={{{2780,0,0},{2999,0,0},false,2780,2999,0},
        {{3000,0,320},{3060,0,320},false,3000,3060,1}};
    seam->landings={{{2700,0,0},5,false,0,0,5},{{3030,0,320},3030,false,1,0,3030}};
    previewSettings.routeDistance=1200;
    previewSettings.pathBatchFrames=defaultFollow.pathBatchFrames;
    previewDisplay.attach(0,seam,seam->frames[2700].position,previewSettings,[](int){});
    assert((previewDisplay.window(0)==line::FrameWindow{0,3000,false}));
    assert(previewDisplay.selected(0)==std::vector<size_t>({0,2}));
    previewDisplay.attach(0,seam,seam->frames[2800].position,previewSettings,[](int){});
    assert((previewDisplay.window(0)==line::FrameWindow{61,3061,false}));
    assert(previewDisplay.selected(0)==std::vector<size_t>({0,1,3}));
    auto& seamEntries=previewDisplay.groups().front()->entries;
    seamEntries[1].visual=42;
    previewDisplay.attach(0,seam,seam->frames[3020].position,previewSettings,[](int){});
    assert((previewDisplay.window(0)==line::FrameWindow{3000,3200,false}));
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,3})&&seamEntries[1].visual==42);

    // Switching content at the same position keeps the current marker handles.
    auto markerOnlySettings=previewSettings;markerOnlySettings.layers={false,false,true};
    seamEntries[3].visual=43;
    std::vector<int> contentStops;
    auto stopContent=[&](int visual){contentStops.push_back(visual);};
    previewDisplay.attach(0,seam,seam->frames[3020].position,markerOnlySettings,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>{3}&&seamEntries[3].visual==43);
    assert(contentStops==std::vector<int>{42});
    previewDisplay.attach(0,seam,seam->frames[3020].position,previewSettings,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,3})&&seamEntries[3].visual==43);
    auto differentRecord=std::make_shared<line::Route>(*seam);
    differentRecord->landings[1].position.y=32;
    previewDisplay.attach(0,differentRecord,seam->frames[3020].position,markerOnlySettings,stopContent);
    assert(contentStops==std::vector<int>({42,43})&&previewDisplay.groups().size()==1);
    assert(previewDisplay.selected(0)==std::vector<size_t>{3});
    previewDisplay.attach(0,differentRecord,seam->frames[2800].position,markerOnlySettings,stopContent);
    assert((previewDisplay.window(0)==line::FrameWindow{61,3061,false}));
    assert(previewDisplay.selected(0)==std::vector<size_t>{3}); // The next landing can appear without its path.
    auto jumpRoute=std::make_shared<line::Route>(*seam);
    jumpRoute->jumps={{{3020,0,320},3020,1,true},{{3040,0,320},3040,1,true},{{3050,0,320},3050,1,false}};
    const auto withJumps=line::buildDrawing(*jumpRoute,previewSettings);
    assert(withJumps.size()==6&&withJumps[4].kind==line::PrimitiveKind::PerfectJump&&withJumps[4].flatMarkerOrigins.size()==2);
    assert(withJumps[5].kind==line::PrimitiveKind::Jump&&withJumps[5].flatMarkerOrigins.size()==1);
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,previewSettings,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,3,4,5}));
    previewDisplay.groups().front()->entries[4].visual=44;
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,markerOnlySettings,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>{3}&&contentStops.back()==44);
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,previewSettings,stopContent);
    auto& jumpEntries=previewDisplay.groups().front()->entries;
    jumpEntries[1].visual=45;
    jumpEntries[3].visual=46;
    jumpEntries[5].visual=47;
    auto hideJumps=previewSettings;
    hideJumps.layers.jumps=false;
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,hideJumps,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,3})&&contentStops.back()==47);
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,previewSettings,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,3,4,5})&&jumpEntries[1].visual==45&&jumpEntries[3].visual==46);
    jumpEntries[4].visual=48;
    auto jumpsOnly=previewSettings;
    jumpsOnly.layers={false,true,false};
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,jumpsOnly,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>({4,5})&&jumpEntries[4].visual==48);
    auto hideAll=previewSettings;
    hideAll.layers={false,false,false};
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,hideAll,stopContent);
    assert(previewDisplay.selected(0).empty()&&contentStops.back()==48);
    previewDisplay.attach(0,jumpRoute,seam->frames[3020].position,previewSettings,stopContent);
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,3,4,5}));

    // Prefer the later return path before the closest replay frame switches sections.
    auto repeated=std::make_shared<line::Route>(*seam);
    for(size_t i=3000;i<repeated->frames.size();++i) repeated->frames[i].position={float(i-220),0,8};
    repeated->path[1]={{2780,0,8},{2840,0,8},false,3000,3060,1};
    repeated->path.push_back({{2840,0,8},{2979,0,8},false,3060,3199,1});
    repeated->landings[1].position={2810,0,8};
    assert(repeated->nearestFrame({2800,0,0})==2800);
    previewDisplay.attach(0,repeated,{2800,0,0},previewSettings,[](int){});
    assert((previewDisplay.window(0)==line::FrameWindow{3000,3200,false}));
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,2}));
    previewDisplay.attach(0,repeated,{2990,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,2}));
    previewDisplay.attach(0,repeated,{2800,0,8},previewSettings,[](int){});
    assert((previewDisplay.window(0)==line::FrameWindow{3000,3200,false}));
    assert(previewDisplay.selected(0)==std::vector<size_t>({1,2,4}));

    auto crossing=std::make_shared<line::Route>(*repeated);
    crossing->path.pop_back();
    crossing->path[1]={{2850,-64,8},{2850,64,8},false,3000,3060,1};
    crossing->landings[1].position={2850,0,8};
    previewDisplay.attach(0,crossing,{2800,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>({0,1,3}));
    auto reverse=std::make_shared<line::Route>(*repeated);
    reverse->path.pop_back();
    std::swap(reverse->path[1].start,reverse->path[1].end);
    previewDisplay.attach(0,reverse,{2800,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>({0,2}));
    auto returned=std::make_shared<line::Route>(*reverse);
    returned->checkpointSections={1};
    previewDisplay.attach(0,returned,{2800,0,0},previewSettings,[](int){});
    assert(previewDisplay.selected(0)==std::vector<size_t>{1});
    // Two sliding windows share their overlap and keep the same native handle.
    auto sliding=std::make_shared<line::Route>();sliding->frames=indexed.frames;sliding->indexFrames();
    sliding->landings={{{0,0,0},0,false,0,0,0},{{8,0,0},8,false,0,0,8},{{16,0,0},16,false,0,0,16}};
    sliding->path={{{0,0,0},{8,0,0},false,0,8,0},{{8,0,0},{16,0,0},false,8,16,0}};
    settings.replayFrames=10;settings.totalElements=4;
    displays.attach(0,sliding,indexed.frames[4].position,settings,erase);assert(displays.selected(0).size()==3);
    displays.attach(1,sliding,indexed.frames[20].position,settings,erase);assert(displays.selected(1).size()==1&&displays.reserved()==4);
    auto* group=displays.groups()[0].get();group->entries[4].visual=99;
    displays.attach(0,sliding,indexed.frames[21].position,settings,erase);
    assert(displays.selected(0).size()==1&&displays.reserved()==1&&group->active.size()==1&&group->entries[4].visual==99&&group->entries[4].subscribers==3);
    displays.clear(0,erase);assert(group->entries[4].visual==99&&group->entries[4].subscribers==2);
    displays.clear(1,erase);assert(displays.reserved()==0);
    // A small alternating strafe is removed, but a turn around a solid corridor is retained.
    std::vector<line::PathSample> wiggles;
    for(uint32_t i=0;i<17;++i) wiggles.push_back({{float(i*8),i==0||i==16?0.0f:(i%2?2.0f:-2.0f),0},i,0,false,i==0||i==16});
    auto smooth=line::simplifyPath(wiggles,6,1,128,100);
    assert(smooth.size()==1&&smooth[0].start.x==0&&smooth[0].end.x==128);
    std::vector<line::PathSample> corner;
    for(uint32_t i=0;i<=8;++i) corner.push_back({{float(i*8),0,0},i,0,false,false});
    for(uint32_t i=1;i<=8;++i) corner.push_back({{64,float(i*8),0},8+i,0,false,false});
    const auto turned=line::simplifyPath(corner,6,1,128,100);assert(turned.size()==2);
    assert(turned[0].end.x==64&&turned[0].end.y==0&&turned[1].start.x==64);
    for(const auto& s:turned) for(int n=0;n<=20;++n) {
        const auto p=s.start+(s.end-s.start)*(n/20.0f);
        assert(!(p.x<56&&p.y>8)); // The old contact-to-contact chord crossed this wall region.
    }
    std::vector<line::PathSample> jumpArc={{{0,0,0},0,0,false,true},{{16,2,20},1,0,false,false},
        {{32,-2,32},2,0,false,false},{{48,2,20},3,0,false,false},{{64,0,0},4,0,false,true}};
    const auto arc=line::simplifyPath(jumpArc,6,1,128,100);
    assert(arc.size()>1&&std::any_of(arc.begin(),arc.end(),[](const line::Segment& s){return s.end.z==32;}));
    std::vector<line::PathSample> stairs={{{0,0,0},0,0,false,true},{{32,0,0},1,0,false,false},
        {{32,0,16},2,0,false,false},{{64,0,16},3,0,false,true}};
    const auto steps=line::simplifyPath(stairs,6,1,128,100);assert(steps.size()==3);
    std::vector<line::PathSample> states={{{0,0,0},0,0,false,true},{{32,0,0},1,0,false,true},
        {{32,0,0},1,0,true,true},{{64,0,0},2,0,true,true},{{64,0,0},2,0,false,true},
        {{96,0,0},3,0,false,true},{{1000,0,0},4,1,false,true},{{1064,0,0},5,1,false,true}};
    const auto classified=line::simplifyPath(states,6,1,128,100);
    assert(classified.size()==4&&!classified[0].crouched&&classified[1].crouched&&!classified[2].crouched);
    assert(classified[2].end.x==96&&classified[3].start.x==1000&&classified[3].section==1);
    // Check the stated XY/Z bounds against every original sample, in replay order.
    for(const auto& samples:std::vector<std::vector<line::PathSample>>{wiggles,corner,jumpArc,stairs,states}) {
        const auto simplified=line::simplifyPath(samples,6,1,128,100);
        for(const auto& p:samples) {
            bool covered=false;
            for(const auto& s:simplified) if(s.section==p.section&&p.frame>=s.firstFrame&&p.frame<=s.lastFrame) {
                const auto d=s.end-s.start,r=p.position-s.start;const float xy=d.x*d.x+d.y*d.y;
                const float t=std::clamp(xy>0.0001f?(r.x*d.x+r.y*d.y)/xy:d.dot(d)>0.0001f?r.dot(d)/d.dot(d):0,0.0f,1.0f);
                const auto error=p.position-(s.start+d*t);
                if(error.x*error.x+error.y*error.y<=36.001f&&std::abs(error.z)<=1.001f) covered=true;
                assert(line::distance2(s.start,s.end)<=128*128+0.01f);
            }
            assert(covered);
        }
    }
    rejects([&]{line::simplifyPath(corner,0,1,128,100);});
    rejects([&]{line::simplifyPath(corner,6,1,128,1);});
    std::atomic<bool> pathCancel{true};rejects([&]{line::simplifyPath(corner,6,1,128,100,&pathCancel);});
    // A broad S remains curved, while dense samples and stance changes do not
    // become hundreds of rendered edges or force a corner at every color switch.
    std::vector<line::PathSample> guide;
    for(uint32_t i=0;i<=128;++i) {
        const float t=i/128.0f;
        guide.push_back({{256*t,32*std::sin(t*6.28318530718f),0},i,0,i>=43&&i<85,false,i==0||i==64||i==128,true});
    }
    size_t guideCurves=0;
    const auto curved=line::smoothPath(guide,6,3,3,256,1000,nullptr,&guideCurves);
    assert(guideCurves>0&&!curved.empty()&&curved.size()<32);
    assert(line::distance2(curved.front().start,guide.front().position)<0.0001f);
    assert(line::distance2(curved.back().end,guide.back().position)<0.0001f);
    assert(std::any_of(curved.begin(),curved.end(),[](const line::Segment& s){return s.end.y>20;}));
    assert(std::any_of(curved.begin(),curved.end(),[](const line::Segment& s){return s.end.y<-20;}));
    assert(std::any_of(curved.begin(),curved.end(),[](const line::Segment& s){return s.crouched;}));
    for(size_t i=0;i<curved.size();++i) {
        assert(curved[i].start.valid()&&curved[i].end.valid()&&curved[i].firstFrame<=curved[i].lastFrame);
        assert(std::abs(curved[i].start.z)<0.001f&&std::abs(curved[i].end.z)<0.001f);
        assert(line::distance2(curved[i].start,curved[i].end)<=256*256+0.01f);
        if(i) assert(line::distance2(curved[i-1].end,curved[i].start)<0.001f);
    }
    for(const auto& p:guide) {
        float closest=1e30f;for(const auto& s:curved) closest=std::min(closest,line::distance2(p.position,s));
        assert(closest<=9*9+0.01f);
        if(p.curveAnchor) assert(std::any_of(curved.begin(),curved.end(),[&](const line::Segment& s){return line::distance2(p.position,s)<0.001f;}));
    }
    const auto coarseGuide=line::smoothPath(guide,6,3,6,256,1000);
    const auto fineGuide=line::smoothPath(guide,6,3,1,256,1000);
    assert(coarseGuide.size()<fineGuide.size());
    const auto roundedCorner=line::smoothPath(corner,6,3,3,256,1000);
    for(const auto& s:roundedCorner) for(int n=0;n<=40;++n) {
        const auto p=s.start+(s.end-s.start)*(n/40.0f);
        assert(!(p.x<54&&p.y>10)); // Do not reintroduce the long chord through the wall.
    }
    // No interpolation between disconnected teleport sections.
    auto disconnected=guide;
    for(size_t i=65;i<disconnected.size();++i) {disconnected[i].section=1;disconnected[i].position.x+=1000;}
    const auto detached=line::smoothPath(disconnected,6,3,3,256,1000);
    for(const auto& s:detached) assert((s.section==0&&s.start.x<1000&&s.end.x<1000)||(s.section==1&&s.start.x>=1000&&s.end.x>=1000));
    const line::Cubic cubic{{0,0,0},{20,30,40},{60,-10,20},{100,0,0}};
    const auto portion=cubic.portion(0.2f,0.8f);
    for(float t:{0.0f,0.25f,0.5f,1.0f}) assert(line::distance2(portion.at(t),cubic.at(0.2f+0.6f*t))<0.0001f);
    rejects([&]{line::smoothPath(guide,6,3,0,256,1000);});
    rejects([&]{line::smoothPath(guide,6,3,3,256,1);});
    rejects([&]{line::smoothPath(guide,6,3,3,256,1000,&pathCancel);});
    std::cout<<"Replay v1-v7, observed landings, exact frames, teleport separation, direct outlines, bounded trajectory simplification, corners, stairs, jump arcs, 3000-frame windows, shared overlap and atomic capacity: passed\n";
}
