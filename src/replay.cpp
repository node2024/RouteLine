// CS2KZ replay wire layout adapted from KZGlobalTeam/cs2kz-metamod, AGPL-3.0.
// See THIRD_PARTY.md for the pinned source and compatibility boundaries.
#include "replay.h"
#include "path.h"
#include "curve.h"
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <zstd.h>

namespace line {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
struct Reader {
    std::string_view data;
    template<class T> T read() {
        if(data.size()<sizeof(T)) fail("Truncated replay data.");
        T out; std::memcpy(&out,data.data(),sizeof(T)); data.remove_prefix(sizeof(T)); return out;
    }
    std::string_view take(size_t n) {
        if(n>data.size()) fail("Truncated replay block.");
        auto out=data.substr(0,n); data.remove_prefix(n); return out;
    }
    uint64_t varint() {
        uint64_t out=0;
        for(unsigned shift=0;shift<64;shift+=7) {
            auto b=read<uint8_t>();
            if(shift==63&&b>1) fail("Invalid protobuf integer.");
            out|=uint64_t(b&127)<<shift;
            if(!(b&128)) return out;
        }
        fail("Invalid protobuf integer.");
    }
};
struct Field { uint32_t number, wire; uint64_t value=0; std::string_view bytes; };
std::vector<Field> fields(std::string_view data) {
    Reader r{data}; std::vector<Field> out;
    while(!r.data.empty()) {
        const auto tag=r.varint();
        Field f{uint32_t(tag>>3),uint32_t(tag&7),0,{}};
        if(!f.number||tag>>3>0x1fffffff) fail("Invalid protobuf tag.");
        switch(f.wire) {
            case 0:f.value=r.varint();break;
            case 1:f.value=r.read<uint64_t>();break;
            case 2:f.bytes=r.take(r.varint());break;
            case 5:f.value=r.read<uint32_t>();break;
            default:fail("Unsupported protobuf wire type.");
        }
        if(out.size()>=65536) fail("Too many protobuf fields.");
        out.push_back(f);
    }
    return out;
}
std::optional<Field> field(std::string_view data,uint32_t n,uint32_t wire) {
    std::optional<Field> found;
    for(auto f:fields(data)) if(f.number==n) { if(f.wire!=wire) fail("Invalid protobuf field type."); found=f; }
    return found;
}
uint64_t integer(std::string_view data,uint32_t n,uint64_t fallback=0) {
    auto f=field(data,n,0); return f?f->value:fallback;
}
std::string_view message(std::string_view data,uint32_t n) { auto f=field(data,n,2); return f?f->bytes:std::string_view{}; }
std::string text(std::string_view data,uint32_t n) { return std::string(message(data,n)); }
float real(std::string_view data,uint32_t n) {
    auto f=field(data,n,5); uint32_t bits=f?uint32_t(f->value):0; float v; std::memcpy(&v,&bits,4); return v;
}
std::string lower(std::string v) { for(auto& c:v) if(c>='A'&&c<='Z') c+=32; return v; }
struct Section { uint64_t count, size; std::string_view bytes; };
Section section(Reader& r,uint32_t version) {
    uint64_t compressed=version>=7?r.read<uint64_t>():r.read<uint32_t>();
    uint64_t size=version>=7?r.read<uint64_t>():r.read<uint32_t>();
    uint64_t count=version>=7?r.read<uint64_t>():r.read<uint32_t>();
    return {count,size,r.take(compressed)};
}
std::string decompress(const Section& s,size_t limit) {
    if(s.size>limit) fail("Replay exceeds the decoded memory limit.");
    std::string result(size_t(s.size),'\0');
    const auto n=ZSTD_decompress(result.data(),result.size(),s.bytes.data(),s.bytes.size());
    if(ZSTD_isError(n)||n!=s.size) fail("Invalid zstd replay section.");
    return result;
}
struct Movement {
    static constexpr uint8_t Walk=2,Ladder=9; // Source 2 MoveType_t values stored on the wire.
    Vec3 origin,velocity;
    uint32_t buttons=0,entityFlags=0;
    float duckAmount=0;
    uint8_t replayFlags=0,moveType=Walk;
    bool grounded() const {return (entityFlags&1)!=0;}
    bool ducked() const {return (replayFlags&2)!=0;}
    bool crouched() const {return (buttons&4)||(replayFlags&7)||duckAmount>0.05f;}
};
struct Tick {
    uint32_t server=0;
    Movement pre,post;
    int32_t teleports=0,lastLandedTick=0;
    float lastLandedFraction=0;
};
constexpr float movementTick=1.0f/64.0f,gravity=800.0f,jumpImpulse=302.0f;
bool hasJumpImpulse(const Movement& after) {
    return after.velocity.z>=jumpImpulse-gravity*movementTick-0.05f&&after.velocity.z<=jumpImpulse+0.05f;
}
// CS2KZ records movement before and after the full 64 Hz physics tick.
// Classic splits movement at the half tick, so both recorded flags can be
// airborne even when landing and takeoff happened between them.
struct TickContact {Vec3 landing;std::optional<Vec3> takeoff;};
std::optional<TickContact> contactWithinTick(const Movement& before,const Movement& after,bool classic) {
    if(before.moveType!=Movement::Walk||after.moveType!=Movement::Walk||before.grounded()
       ||before.velocity.z>=0) return std::nullopt;

    constexpr float tick=movementTick;
    const Vec3 start{before.origin.x,before.origin.y,0};
    const Vec3 end{after.origin.x,after.origin.y,0};
    const Vec3 incoming{before.velocity.x,before.velocity.y,0};
    auto fallTime=[&](float drop) {
        const float speed=-before.velocity.z;
        return 2*drop/(speed+std::sqrt(speed*speed+2*gravity*drop));
    };
    if(after.grounded()) {
        const float drop=before.origin.z-after.origin.z;
        if(drop<=0||std::abs(after.velocity.z)>1||before.velocity.z>=-20
           ||before.crouched()!=after.crouched()) return std::nullopt;
        const float time=fallTime(drop);
        if(time>=tick) return std::nullopt;

        // Estimate where the falling path reached the recorded ground height.
        // Only rewind a small amount along the recorded movement direction.
        const Vec3 travel=end-start;
        const float lengthSquared=travel.dot(travel);
        if(lengthSquared<0.25f) return std::nullopt;
        const Vec3 predicted=start+incoming*time;
        const float fraction=(predicted-start).dot(travel)/lengthSquared;
        if(fraction<0||fraction>=1) return std::nullopt;
        const Vec3 contact=start+travel*fraction;
        const float correction=distance2(contact,end);
        if(distance2(contact,predicted)>1||correction<0.25f||correction>16) return std::nullopt;
        return TickContact{{contact.x,contact.y,after.origin.z},std::nullopt};
    }

    constexpr float half=tick/2;
    if(!classic||(before.crouched()!=after.crouched()&&before.ducked()!=after.ducked())
       ||after.velocity.z<jumpImpulse-gravity*tick||after.velocity.z>jumpImpulse)
        return std::nullopt;
    const float floor=after.origin.z-(after.velocity.z+gravity*half/2)*half;
    const float approach=before.origin.z+(before.velocity.z-gravity*half/2)*half;
    // Half a tick gives the takeoff estimate. A fast fall can reach the floor
    // earlier, with the rest of that movement step spent on the platform.
    constexpr float heightTolerance=2.5f;
    if(floor>=before.origin.z||floor<approach-heightTolerance) return std::nullopt;

    const Vec3 outgoing{after.velocity.x,after.velocity.y,0};
    const Vec3 fromBefore=start+incoming*half;
    const Vec3 fromAfter=end-outgoing*half;
    const Vec3 takeoff=(fromBefore+fromAfter)*0.5f;
    const float contactFraction=floor>approach+heightTolerance?fallTime(before.origin.z-floor)/half:1.0f;
    const Vec3 contact=start+(takeoff-start)*contactFraction;
    if(distance2(fromBefore,fromAfter)>4||distance2(contact,Segment{start,end})>1) return std::nullopt;
    return TickContact{{contact.x,contact.y,floor},Vec3{takeoff.x,takeoff.y,floor}};
}
// Airborne samples can hide a contact near the apex or during uncrouch.
// Collect plausible positions here and require standable map geometry before drawing them.
std::optional<Vec3> landingCandidate(const Movement& before,const Movement& after) {
    if(before.moveType!=Movement::Walk||after.moveType!=Movement::Walk||before.grounded()||after.grounded())
        return std::nullopt;
    const bool jumped=before.velocity.z<=32&&after.velocity.z>=jumpImpulse-gravity*movementTick-0.05f
        &&after.velocity.z<=jumpImpulse;
    const bool stoppedFalling=before.velocity.z<-20&&after.velocity.z>=-6.3f&&after.velocity.z<=0.05f
        &&before.ducked()==after.ducked();
    if(!jumped&&!stoppedFalling) return std::nullopt;

    constexpr float half=movementTick/2;
    const float floor=jumped?after.origin.z-(after.velocity.z+gravity*half/2)*half:after.origin.z;
    const float drop=before.origin.z-floor;
    const float fallDistance=(std::max(0.0f,-before.velocity.z)+gravity*half)*movementTick;
    const float uncrouch=before.ducked()&&!after.ducked()?9.0f:0.0f;
    if(drop<(jumped?-2.0f:0.0f)||drop>fallDistance+2.5f+uncrouch) return std::nullopt;
    const auto delta=after.origin-before.origin;
    const float speed=std::max(std::hypot(before.velocity.x,before.velocity.y),std::hypot(after.velocity.x,after.velocity.y));
    if(std::hypot(delta.x,delta.y)>speed*movementTick+2) return std::nullopt;
    return Vec3{before.origin.x+delta.x/2,before.origin.y+delta.y/2,floor};
}
void movement(Reader& r,uint64_t mask,unsigned offset,Movement& m) {
    constexpr size_t sizes[]={12,12,12,4,4,4,4,4,4,4,4,1,4,1};
    for(unsigned n=0;n<14;++n) if(mask&(uint64_t(1)<<(offset+n))) {
        if(n==0) m.origin=r.read<Vec3>();
        else if(n==1) m.velocity=r.read<Vec3>();
        else if(n==3) m.buttons=r.read<uint32_t>();
        else if(n==8) m.duckAmount=r.read<float>();
        else if(n==11) m.replayFlags=r.read<uint8_t>();
        else if(n==12) m.entityFlags=r.read<uint32_t>();
        else if(n==13) m.moveType=r.read<uint8_t>();
        else r.take(sizes[n]);
    }
}
std::vector<Tick> ticks(const Section& s,uint32_t version,const DecodeOptions& o,const std::atomic<bool>* cancel) {
    if(!s.count||s.count>o.maxTicks||s.count>s.size/8) fail("Invalid or oversized replay tick count.");
    auto bytes=decompress(s,o.maxDecodedBytes); Reader r{bytes};
    std::vector<Tick> out; out.reserve(size_t(s.count)); Tick tick;
    for(size_t i=0;i<s.count;++i) {
        if(cancel&&cancel->load()) fail("Replay loading cancelled.");
        auto flags=r.read<uint64_t>();
        const unsigned lastBit=version==1?39:version==2?41:42;
        if(flags>>(lastBit+1)) fail("Unknown replay movement fields.");
        if(i&&!(flags&1)) ++tick.server;
        if(flags&1) tick.server=r.read<uint32_t>();
        constexpr size_t topSizes[]={4,4,4,8,4,4,4,4,4,1};
        for(unsigned n=1;n<10;++n) if(flags&(uint64_t(1)<<n)) r.take(topSizes[n]);
        if(flags&(uint64_t(1)<<39)) r.take(4); // Upstream v2 also uses bit 39 for modern-jump actual press.
        tick.pre=i?tick.post:Movement{};
        movement(r,flags,10,tick.pre); tick.post=tick.pre; movement(r,flags,24,tick.post);
        if(flags&(uint64_t(1)<<38)) { r.take(8); tick.teleports=r.read<int32_t>(); }
        if(version>=2) {
            unsigned first=version==2?39:40;
            if(flags&(uint64_t(1)<<first)) r.take(8);
            if(flags&(uint64_t(1)<<(first+1))) r.take(8);
            if(flags&(uint64_t(1)<<(first+2))) {
                tick.lastLandedTick=r.read<int32_t>();tick.lastLandedFraction=r.read<float>();r.take(12);
            }
        }
        if(!tick.pre.origin.valid()||!tick.post.origin.valid()||!tick.pre.velocity.valid()||!tick.post.velocity.valid()
           ||!std::isfinite(tick.pre.duckAmount)||!std::isfinite(tick.post.duckAmount)
           ||!std::isfinite(tick.lastLandedFraction)||tick.teleports<0||(!out.empty()&&tick.server<out.back().server))
            fail("Invalid replay movement sample.");
        out.push_back(tick);
    }
    if(!r.data.empty()) fail("Unexpected bytes in replay tick section.");
    return out;
}
// Merge only connected, equally classified segments. Checking every skipped vertex keeps
// curves within lineError; the length cap preserves local visibility and spatial lookup accuracy.
void simplify(Route& route,float error,float maxLength) {
    if(error<=0||route.segments.empty()) return;
    const auto& input=route.segments;std::vector<Segment> out;out.reserve(input.size());
    size_t first=0,last=0;
    while(first<input.size()) {
        last=first;float arcLength=std::sqrt(distance2(input[first].start,input[first].end));
        while(last+1<input.size()&&input[last+1].crouched==input[first].crouched
              &&last-first<255&&distance2(input[last].end,input[last+1].start)<0.0001f) {
            Segment candidate{input[first].start,input[last+1].end,input[first].crouched};
            const float nextLength=std::sqrt(distance2(input[last+1].start,input[last+1].end));
            if(arcLength+nextLength>maxLength||distance2(candidate.start,candidate.end)>maxLength*maxLength) break;
            bool fits=true;
            for(size_t j=first;j<=last;++j) if(distance2(input[j].end,candidate)>error*error) {fits=false;break;}
            if(!fits) break;
            ++last;arcLength+=nextLength;
        }
        out.push_back({input[first].start,input[last].end,input[first].crouched});first=last+1;
    }
    route.segments=std::move(out);
}
struct Event {
    static constexpr uint64_t CheckpointReturn=0;
    uint32_t type=0,tick=0,timerType=0;
    int32_t course=0;
    float time=0;
    uint64_t action=UINT64_MAX,checkpoint=UINT64_MAX;
    std::optional<Vec3> destination;
};
std::optional<Vec3> eventOrigin(std::string_view payload,uint32_t number) {
    const auto origin=field(payload,number,2);
    if(!origin) return std::nullopt;
    const Vec3 position{real(origin->bytes,1),real(origin->bytes,2),real(origin->bytes,3)};
    return position.valid()?std::optional(position):std::nullopt;
}
std::vector<Event> events(const Section& s,uint32_t version,const DecodeOptions& o) {
    if(s.count>o.maxTicks||s.count>s.size/(version>=6?4:172)) fail("Invalid replay event count.");
    auto bytes=decompress(s,o.maxDecodedBytes); Reader r{bytes}; std::vector<Event> out;
    for(size_t i=0;i<s.count;++i) {
        Event e;
        if(version>=6) {
            auto b=r.take(r.read<uint32_t>());
            e.type=uint32_t(integer(b,1,UINT32_MAX)); e.tick=uint32_t(integer(b,2));
            if(e.type==0) {
                auto timer=message(b,4); e.timerType=uint32_t(integer(timer,1,UINT32_MAX));
                e.course=int32_t(integer(timer,2)); e.time=real(timer,3);
            } else if(e.type==3||e.type==4) {
                const auto payload=message(b,e.type==3?7:8);
                e.action=integer(payload,1,UINT64_MAX);
                e.checkpoint=integer(payload,2,UINT64_MAX);
                e.destination=eventOrigin(payload,e.type==3?4:3);
            }
        } else {
            Reader old{r.take(172)}; e.type=old.read<uint32_t>(); e.tick=old.read<uint32_t>();
            if(e.type==0) { e.timerType=old.read<uint32_t>(); e.course=old.read<int32_t>(); e.time=old.read<float>(); }
            if(e.type==3&&old.read<uint8_t>()==1) {
                old.take(3); // hasAngles, hasVelocity and padding in legacy replay events.
                const auto position=old.read<Vec3>();
                if(position.valid()) e.destination=position;
            }
        }
        if(!out.empty()&&e.tick<out.back().tick) fail("Replay events are out of order.");
        out.push_back(e);
    }
    if(!r.data.empty()) fail("Unexpected bytes in replay event section.");
    return out;
}

struct AttemptCut {
    uint32_t begin,end; // Half-open range of original server ticks.
    bool repeatedReturn=false;
    bool matchedDeparture=false;
};
auto sampleAt(const std::vector<Tick>& samples,uint32_t serverTick) {
    return std::lower_bound(samples.begin(),samples.end(),serverTick,
        [](const Tick& tick,uint32_t server){return tick.server<server;});
}
bool movedByTeleport(const Event& event,const std::vector<Tick>& samples) {
    const auto sample=sampleAt(samples,event.tick);
    if(sample==samples.begin()||sample==samples.end()||sample->server!=event.tick
       ||(sample-1)->server+1!=event.tick) return false;
    const auto destination=*event.destination;
    // An origin payload can be a no-op. Require an observed arrival from a different position.
    return distance2((sample-1)->post.origin,destination)>16*16
        &&(distance2(sample->pre.origin,destination)<=16*16||distance2(sample->post.origin,destination)<=16*16
           ||(sample+1!=samples.end()&&(sample+1)->server==event.tick+1
              &&distance2((sample+1)->pre.origin,destination)<=16*16));
}
void matchInitialDeparture(AttemptCut& cut,const std::vector<Tick>& samples,uint32_t before,uint32_t after) {
    // The first failure can precede the first return. Compare departures from
    // the same platform in the first approach and the last attempt.
    const auto firstBegin=sampleAt(samples,std::max(before,cut.begin-std::min(cut.begin,256u)));
    const auto firstEnd=sampleAt(samples,cut.begin);
    const auto lastBegin=sampleAt(samples,cut.end+1);
    const auto lastEnd=sampleAt(samples,cut.end+std::min(after-cut.end,128u));
    constexpr int matchTicks=4;
    auto departure=[](auto tick) {
        return (tick-1)->server+1==tick->server&&(tick-1)->post.grounded()&&!tick->post.grounded()
            &&(tick-1)->post.moveType==2&&tick->post.moveType==2;
    };
    if(firstEnd-firstBegin<=matchTicks||lastEnd-lastBegin<=matchTicks) return;
    float best=8*8*matchTicks;
    for(auto first=firstBegin+1;first<=firstEnd-matchTicks;++first) {
        if(!departure(first)) continue;
        for(auto last=lastBegin+1;last<=lastEnd-matchTicks;++last) {
            if(!departure(last)) continue;
            float error=0;
            bool matches=true;
            for(int step=0;step<matchTicks;++step) {
                const auto& a=first[step].post;
                const auto& b=last[step].post;
                const auto delta=a.origin-b.origin;
                const float speedProduct=a.velocity.dot(a.velocity)*b.velocity.dot(b.velocity);
                if(first[step].server!=first->server+step||last[step].server!=last->server+step
                   ||a.moveType!=2||b.moveType!=2||a.grounded()||b.grounded()||std::abs(delta.z)>2
                   ||delta.dot(delta)>8*8||speedProduct<=0||a.velocity.dot(b.velocity)<0.95f*std::sqrt(speedProduct)) {
                    matches=false;
                    break;
                }
                error+=delta.dot(delta);
            }
            if(matches&&error<best) {
                best=error;
                cut={first->server,last->server,true,true};
            }
        }
    }
}
std::vector<AttemptCut> failedAttempts(const std::vector<Event>& events,const std::vector<Tick>& samples,
                                       uint32_t start,uint32_t end,
                                       const std::atomic<bool>* cancel) {
    constexpr uint64_t saveCheckpoint=0,resetCheckpoints=1,undoCheckpoint=1;
    std::map<uint64_t,const Event*> checkpoints;
    const Event* lastTeleport=nullptr;
    std::vector<AttemptCut> cuts;
    std::vector<uint32_t> teleportTicks;
    for(size_t i=0;i<events.size();) {
        if(cancel&&cancel->load()) fail("Replay loading cancelled.");
        const auto tick=events[i].tick;
        if(tick>=end) break;
        const Event* arrival=nullptr;
        bool hasTeleport=false;
        // Only the final position teleport in this tick determines where the next attempt starts.
        for(;i<events.size()&&events[i].tick==tick;++i) {
            const auto& event=events[i];
            if(tick<start) continue;
            if(event.type==4) {
                if(event.action==resetCheckpoints) checkpoints.clear();
                if(event.action==saveCheckpoint&&event.destination&&event.checkpoint<=INT32_MAX)
                    checkpoints[event.checkpoint]=&event;
            }
            if(event.type==3) {
                hasTeleport=true;
                if(event.destination) arrival=&event;
            }
        }
        if(hasTeleport) teleportTicks.push_back(tick);
        if(!arrival) continue;
        if(!movedByTeleport(*arrival,samples)) {
            lastTeleport=nullptr;
            continue;
        }

        uint32_t rewind=tick;
        const bool repeatedReturn=arrival->action!=undoCheckpoint&&lastTeleport&&lastTeleport->action!=undoCheckpoint
            &&distance2(*arrival->destination,*lastTeleport->destination)<0.0001f;
        if(repeatedReturn) rewind=lastTeleport->tick;
        if(arrival->action==Event::CheckpointReturn) {
            const auto saved=checkpoints.find(arrival->checkpoint);
            if(saved!=checkpoints.end()&&saved->second->tick<tick
               &&distance2(*arrival->destination,*saved->second->destination)<0.0001f) {
                const auto savedTick=saved->second->tick;
                auto cut=std::upper_bound(cuts.begin(),cuts.end(),savedTick,
                    [](uint32_t t,const AttemptCut& range){return t<range.end;});
                // A checkpoint saved on an already discarded branch cannot anchor the retained route.
                if(cut==cuts.end()||savedTick<cut->begin) rewind=std::min(rewind,savedTick+1);
            }
        }
        bool canMatchDeparture=repeatedReturn&&rewind==lastTeleport->tick;
        lastTeleport=arrival;
        if(rewind>=tick) continue;
        while(!cuts.empty()&&cuts.back().end>=rewind) {
            canMatchDeparture=canMatchDeparture&&cuts.back().repeatedReturn&&cuts.back().end==rewind;
            rewind=std::min(rewind,cuts.back().begin);
            cuts.pop_back();
        }
        cuts.push_back({rewind,tick,canMatchDeparture});
    }
    for(size_t i=0;i<cuts.size();++i) {
        if(cancel&&cancel->load()) fail("Replay loading cancelled.");
        auto& cut=cuts[i];
        if(!cut.repeatedReturn) continue;
        const auto first=std::lower_bound(teleportTicks.begin(),teleportTicks.end(),cut.begin);
        const auto next=std::upper_bound(teleportTicks.begin(),teleportTicks.end(),cut.end);
        auto before=first==teleportTicks.begin()?start:*(first-1)+1;
        auto after=next==teleportTicks.end()?end:*next;
        if(i) before=std::max(before,cuts[i-1].end+1);
        if(i+1<cuts.size()) after=std::min(after,cuts[i+1].begin);
        matchInitialDeparture(cut,samples,before,after);
    }
    return cuts;
}
} // namespace
Route decodeReplay(std::string_view bytes,const Query& query,const Record& record,const DecodeOptions& options,const std::atomic<bool>* cancel) {
    if(bytes.size()>options.maxFileBytes) fail("Replay file exceeds the configured limit.");
    Reader r{bytes}; auto headerSize=r.read<uint32_t>();
    if(!headerSize||headerSize>5*1024*1024) fail("Invalid replay header size.");
    auto h=r.take(headerSize); const auto version=uint32_t(integer(h,1));
    if(version<1||version>7) fail("Unsupported CS2KZ replay version (supported: 1-7).");
    if(integer(h,2,UINT32_MAX)!=2) fail("The selected replay is not a run replay.");
    auto run=message(h,16), mode=message(run,2);
    const auto map=text(message(h,4),1), course=text(run,1);
    const auto modeName=lower(text(mode,1)), shortName=lower(text(mode,2));
    const bool correctMode=query.mode=="classic"?(modeName=="classic"||shortName=="ckz"):(modeName=="vanilla"||shortName=="vnl");
    if(map!=query.map||course!=query.course||!correctMode) fail("Replay map, course or mode does not match the selected record.");
    for(auto f:fields(run)) if(f.number==3) fail("Styled replays are not supported by this CS2KZ provider.");
    const auto duration=real(run,4); const auto tp=integer(run,5);
    if(!std::isfinite(duration)||duration<=0||tp>INT32_MAX||(query.pro&&tp)) fail("Invalid replay time or PRO category.");
    if(record.time>0&&std::abs(record.time-duration)>0.1) fail("Replay time does not match the selected record.");
    auto samples=ticks(section(r,version),version,options,cancel);
    section(r,version); // Subticks (positions are already in movement ticks).
    section(r,version); // Weapons.
    section(r,version); // Jumpstats.
    auto ev=events(section(r,version),version,options);
    // Select the last completed run matching the header. The pre-run buffer may contain earlier runs.
    std::optional<uint32_t> start,end;
    for(size_t i=ev.size();i-->0;) if(ev[i].type==0&&ev[i].timerType==1&&std::isfinite(ev[i].time)&&std::abs(ev[i].time-duration)<0.1) {
        for(size_t j=i;j-->0;) if(ev[j].type==0&&(ev[j].timerType==0||ev[j].timerType==1||ev[j].timerType==2)) {
            if(ev[j].timerType==0&&ev[j].course==ev[i].course) { start=ev[j].tick;end=ev[i].tick; }
            break;
        }
        if(start) break;
    }
    if(!start||!end||*end<*start) fail("Replay has no matching timer start/end events.");
    const auto cuts=options.hideFailedAttempts?failedAttempts(ev,samples,*start,*end,cancel):std::vector<AttemptCut>{};
    auto cut=cuts.begin();
    Route route; route.query=query;route.record=record;route.replayVersion=version;
    std::optional<Vec3> previous;
    Tick previousTick{};
    bool havePreviousTick=false;
    bool previousCrouched=false;
    const bool classic=query.mode=="classic";
    std::optional<uint32_t> lastClassicLanding;
    uint32_t sectionId=0,windowSection=0;
    std::vector<PathSample> pathSamples;
    pathSamples.reserve(samples.size()*2);
    int32_t teleports=0;bool first=true;size_t eventIndex=0;
    auto add=[&](Vec3 pos,bool crouched) {
        if(previous) {
            auto d=distance2(*previous,pos);
            if(d>options.maxStep*options.maxStep) {previous=pos;previousCrouched=crouched;}
            else if(d>=options.spacing*options.spacing||crouched!=previousCrouched) {
                if(d>0.0001f) {
                    if(route.segments.size()>=options.maxSegments) fail("Route exceeds the configured segment limit.");
                    route.segments.push_back({*previous,pos,previousCrouched});
                }
                previous=pos;previousCrouched=crouched;
            }
        } else {previous=pos;previousCrouched=crouched;}
    };
    auto observe=[&](const Movement& current,const Movement* prior,uint32_t serverTick,uint8_t phase) {
        const bool recordedContact=current.grounded()&&(!prior||!prior->grounded());
        // CS2KZ also calls RegisterLanding when an airborne player attaches to a ladder.
        const bool ladderContact=prior&&prior->moveType==Movement::Walk&&current.moveType==Movement::Ladder
            &&!prior->grounded()&&!current.grounded();
        const auto contact=phase==1&&prior?contactWithinTick(*prior,current,classic):std::nullopt;
        const auto frame=uint32_t(route.frames.size()-1);
        if(recordedContact&&phase==1&&prior&&prior->moveType==Movement::Walk&&current.moveType==Movement::Walk)
            lastClassicLanding=serverTick;
        const bool hiddenHop=contact&&contact->takeoff;
        const bool groundJump=prior&&prior->grounded()&&!current.grounded()&&prior->moveType==Movement::Walk
            &&current.moveType==Movement::Walk&&hasJumpImpulse(current);
        if(hiddenHop||groundJump) {
            // Classic places landings at the end of a half step and takeoffs
            // at its start. Adjacent full ticks are at most 15.625 ms apart,
            // inside Classic's 20 ms perf window. Vanilla's internal
            // WalkMove state (used for its perf check) is not recorded.
            const bool perfect=classic&&(hiddenHop||(lastClassicLanding&&serverTick-*lastClassicLanding<=1));
            route.jumps.push_back({hiddenHop?*contact->takeoff:prior->origin,frame,sectionId,perfect});
        }
        if(!current.grounded()||current.moveType!=Movement::Walk) lastClassicLanding.reset();
        if(contact) {
            // Keep the reconstructed contact in both path styles, so the guide
            // and marker meet at the same position.
            pathSamples.push_back({contact->landing,frame,sectionId,prior->crouched(),true,true,true});
            if(!recordedContact) ++route.recoveredLandings;
        }
        const bool anchor=!prior||recordedContact||ladderContact||prior->crouched()!=current.crouched();
        const bool curveAnchor=!prior||recordedContact||ladderContact
            ||(!prior->grounded()&&!current.grounded()&&prior->velocity.z>0&&current.velocity.z<=0);
        // Preserve the last ground sample at takeoff, and the first ground sample
        // at landing. Avoid forcing both adjacent air/ground samples into the line.
        if(prior&&prior->grounded()&&!current.grounded()&&!pathSamples.empty()&&pathSamples.back().section==sectionId) {
            pathSamples.back().anchor=true;pathSamples.back().curveAnchor=true;
        }
        pathSamples.push_back({current.origin,frame,sectionId,current.crouched(),anchor,curveAnchor,current.grounded()});
        if(recordedContact||ladderContact||contact) {
            if(route.landings.size()>=options.maxLandings) fail("Replay exceeds the configured landing marker limit.");
            const bool crouchedOnContact=current.crouched()||(prior&&prior->crouched());
            route.landings.push_back({contact?contact->landing:current.origin,serverTick,crouchedOnContact,sectionId,phase,frame,ladderContact});
        } else if(phase==1&&prior&&route.landingCandidates.size()<256) {
            if(const auto candidate=landingCandidate(*prior,current))
                route.landingCandidates.push_back({*candidate,serverTick,prior->crouched()||current.crouched(),sectionId,phase,frame,false,hasJumpImpulse(current)});
        }
    };
    for(const auto& tick:samples) {
        if(cancel&&cancel->load()) fail("Replay loading cancelled.");
        if(tick.server<*start||tick.server>*end) continue;
        if(tick.pre.moveType==7||tick.post.moveType==7) fail("Run contains noclip movement.");
        bool matchedDeparture=false;
        while(cut!=cuts.end()&&cut->end<=tick.server) {
            matchedDeparture=cut->matchedDeparture&&cut->end==tick.server;
            ++cut;
        }
        if(cut!=cuts.end()&&cut->begin<=tick.server) {
            ++route.discardedAttemptFrames;
            continue;
        }
        route.frames.push_back({tick.post.origin,tick.server,windowSection});
        bool teleport=false;
        const Event* arrival=nullptr;
        while(eventIndex<ev.size()&&ev[eventIndex].tick<=tick.server) {
            const auto& event=ev[eventIndex];
            if(event.tick==tick.server&&event.type==3) {
                teleport=true;
                if(event.destination) arrival=&event;
            }
            ++eventIndex;
        }
        // Entire teleport ticks are disconnected; never draw a link across a map/CP teleport.
        const bool gap=havePreviousTick&&tick.server!=previousTick.server+1;
        const bool warp=distance2(tick.pre.origin,tick.post.origin)>options.maxStep*options.maxStep
            ||(havePreviousTick&&distance2(previousTick.post.origin,tick.pre.origin)>options.maxStep*options.maxStep);
        bool disconnected=teleport||(!first&&tick.teleports!=teleports)||gap||warp;
        if(disconnected||matchedDeparture) {
            previous.reset();lastClassicLanding.reset();havePreviousTick=false;++sectionId;
        }
        if(disconnected&&!matchedDeparture) {
            // Teleport phase differs between recordings. Dropping this entire tick also prevents
            // a short AFTER_PHYSICS teleport from linking its pre-teleport post origin to the next tick.
            route.frames.back().section=++windowSection;
            if(arrival&&arrival->action==Event::CheckpointReturn&&movedByTeleport(*arrival,samples))
                route.checkpointSections.push_back(windowSection);
            teleports=tick.teleports;first=false;continue;
        }
        observe(tick.pre,matchedDeparture?&tick.pre:havePreviousTick?&previousTick.post:nullptr,tick.server,0);
        observe(tick.post,&tick.pre,tick.server,1);
        add(tick.pre.origin,tick.pre.crouched());add(tick.post.origin,tick.post.crouched());
        teleports=tick.teleports;first=false;previousTick=tick;havePreviousTick=true;
    }
    if(route.segments.empty()) fail("Replay contains no drawable run route.");
    if(route.landingCandidates.size()>options.maxLandings-route.landings.size())
        route.landingCandidates.resize(options.maxLandings-route.landings.size());
    simplify(route,options.lineError,std::min(options.maxStep,128.0f));
    route.path=options.smoothPath?smoothPath(pathSamples,options.pathXYError,options.pathZError,options.curveFlatness,
        options.maxPathLength,options.maxSegments,cancel,&route.pathCurves):
        simplifyPath(pathSamples,options.pathXYError,options.pathZError,options.maxPathLength,options.maxSegments,cancel);
    route.indexFrames();
    return route;
}
} // namespace line
