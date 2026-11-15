#pragma once
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace line {
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(Vec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec3 operator-(Vec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec3 operator*(float n) const { return {x*n,y*n,z*n}; }
    float dot(Vec3 b) const { return x*b.x+y*b.y+z*b.z; }
    bool valid() const { return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z)&&std::abs(x)<1000000&&std::abs(y)<1000000&&std::abs(z)<1000000; }
};
inline float distance2(Vec3 a, Vec3 b) { const auto d=a-b; return d.dot(d); }
struct Segment {Vec3 start,end;bool crouched=false;uint32_t firstFrame=0,lastFrame=0,section=0;};
struct PathSample {Vec3 position;uint32_t frame=0,section=0;bool crouched=false,anchor=false,curveAnchor=false,grounded=false;};
struct Landing {
    Vec3 position;
    uint32_t serverTick=0;
    bool crouched=false;
    uint32_t section=0;
    uint8_t phase=0;
    uint32_t frameIndex=0;
    bool ladder=false;
    bool followedByJump=false; // Only used by unconfirmed same-tick contacts.
    uint64_t sample() const {return uint64_t(serverTick)*2+phase;}
};
struct Jump {Vec3 position;uint32_t frameIndex=0,section=0;bool perfect=false;};
// A display window may span a matched retry seam while path segments remain disconnected.
struct ReplayFrame {Vec3 position;uint32_t serverTick=0,section=0;};
inline float distance2(Vec3 point, const Segment& s) {
    const auto delta=s.end-s.start;
    const auto n=delta.dot(delta);
    return distance2(point,s.start+delta*(n>0?std::clamp((point-s.start).dot(delta)/n,0.0f,1.0f):0));
}
struct Query {
    std::string map, course = "main", mode = "classic";
    bool global = true, pro = true;
    unsigned rank = 1;
};
inline constexpr unsigned MaxRecordRank=1000;
// Only consume rank arguments immediately after a record source. This leaves
// a numeric course name usable elsewhere in the command.
inline std::optional<unsigned> recordRankArgument(std::string_view value) {
    auto digits=value;
    if(!digits.empty()&&(digits.front()=='+'||digits.front()=='-')) digits.remove_prefix(1);
    if(digits.empty()||digits.find_first_not_of("0123456789")!=std::string_view::npos) return std::nullopt;
    unsigned rank=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),rank);
    if(parsed.ec!=std::errc()||rank<1||rank>MaxRecordRank)
        throw std::invalid_argument("Rank must be a number from 1 to 1000.");
    return rank;
}
struct Record { std::string id, player; double time = 0; int teleports = 0; };
struct Route {
    Query query;
    Record record;
    uint32_t replayVersion = 0;
    std::vector<Segment> segments;
    std::vector<Segment> path;
    size_t pathCurves=0;
    size_t recoveredLandings=0;
    size_t discardedAttemptFrames=0;
    std::vector<Landing> landings;
    std::vector<Landing> landingCandidates;
    std::vector<Jump> jumps;
    std::vector<ReplayFrame> frames;
    std::vector<uint32_t> checkpointSections;
    template<class Probe> void confirmLandings(Probe groundHeight) {
        const auto recorded=landings.size();
        const auto recordedJumps=jumps.size();
        for(auto candidate:landingCandidates) {
            if(const auto height=groundHeight(candidate.position)) {
                candidate.position.z=*height;
                landings.push_back(candidate);
                if(candidate.followedByJump) jumps.push_back({candidate.position,candidate.frameIndex,candidate.section,false});
            }
        }
        landingCandidates.clear();
        recoveredLandings+=landings.size()-recorded;
        if(landings.size()!=recorded) std::inplace_merge(landings.begin(),landings.begin()+recorded,landings.end(),
            [](const Landing& a,const Landing& b){return a.frameIndex*2+a.phase<b.frameIndex*2+b.phase;});
        if(jumps.size()!=recordedJumps) std::inplace_merge(jumps.begin(),jumps.begin()+recordedJumps,jumps.end(),
            [](const Jump& a,const Jump& b){return a.frameIndex<b.frameIndex;});
    }
    struct Cell {int x,y,z;bool operator==(const Cell& b) const {return x==b.x&&y==b.y&&z==b.z;}};
    struct CellHash {size_t operator()(const Cell& c) const {return uint32_t(c.x)*73856093U^uint32_t(c.y)*19349663U^uint32_t(c.z)*83492791U;}};
    std::unordered_map<Cell,std::vector<size_t>,CellHash> frameGrid;
    static Cell cell(Vec3 p) {return {int(std::floor(p.x/256)),int(std::floor(p.y/256)),int(std::floor(p.z/256))};}
    void indexFrames() {frameGrid.clear();for(size_t i=0;i<frames.size();++i) frameGrid[cell(frames[i].position)].push_back(i);}
    size_t nearestFrame(Vec3 position) const {
        if(frames.empty()||!position.valid()) return 0;
        size_t best=0;float distance=distance2(position,frames[0].position);
        auto consider=[&](size_t i) {const auto d=distance2(position,frames[i].position);if(d<distance||(d==distance&&i<best)) {distance=d;best=i;}};
        if(frameGrid.empty()) {for(size_t i=1;i<frames.size();++i) consider(i);return best;}
        if(auto it=frameGrid.find(cell(position));it!=frameGrid.end()) for(auto i:it->second) consider(i);
        for(const auto& [c,indices]:frameGrid) {
            const Vec3 lo{float(c.x*256),float(c.y*256),float(c.z*256)};
            const Vec3 nearest{std::clamp(position.x,lo.x,lo.x+256),std::clamp(position.y,lo.y,lo.y+256),std::clamp(position.z,lo.z,lo.z+256)};
            if(distance2(position,nearest)<=distance) for(auto i:indices) consider(i);
        }
        return best;
    }
};
// Timer backends only resolve records and replay bytes; the game renderer consumes Route.
// All callbacks run on the game thread. A backend must cancel/drain its callbacks in shutdown.
struct DecodeOptions;
class IRouteProvider {
public:
    using DecodeFunction=Route(*)(std::string_view,const Query&,const Record&,const DecodeOptions&,const std::atomic<bool>*);
    // A backend supplies its own decoder. No CS2KZ replay details enter the renderer.
    struct Input { std::string path, bytes; DecodeFunction decode=nullptr; };
    using BytesCallback=std::function<void(Query,Record,Input,std::string)>;
    virtual ~IRouteProvider()=default;
    virtual void request(const Query&,BytesCallback)=0;
    virtual void frame()=0;
    virtual void shutdown()=0;
};
inline bool uuid(const std::string& id) {
    if(id.size()!=36) return false;
    for(size_t i=0;i<id.size();++i) {
        if(i==8||i==13||i==18||i==23) { if(id[i]!='-') return false; }
        else if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')||(id[i]>='A'&&id[i]<='F'))) return false;
    }
    return true;
}
inline std::string encode(const std::string& value) {
    constexpr char hex[]="0123456789ABCDEF";
    std::string out;
    for(unsigned char c:value) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out+=char(c);
        else { out+='%'; out+=hex[c>>4]; out+=hex[c&15]; }
    }
    return out;
}
inline std::string recordsURL(const std::string& api,const Query& q) {
    return api+"/records?top=true&map="+encode(q.map)+"&course="+encode(q.course)+"&mode="+q.mode
        +(q.pro?"&has_teleports=false":"")+"&sort_by=time&sort_order=ascending&limit=1&offset="+std::to_string(q.rank-1);
}
inline std::string recordSQL(const Query& q,const std::function<std::string(const std::string&)>& escape) {
    return "SELECT t.ID,p.Alias,t.RunTime,t.Teleports FROM Times t "
        "JOIN MapCourses mc ON mc.ID=t.MapCourseID JOIN Maps m ON m.ID=mc.MapID "
        "JOIN Modes mo ON mo.ID=t.ModeID JOIN Players p ON p.SteamID64=t.SteamID64 "
        "WHERE t.StyleIDFlags=0 AND m.Name='"+escape(q.map)+"' AND mc.Name='"+escape(q.course)
        +"' AND mo.ShortName='"+(q.mode=="classic"?"CKZ":"VNL")+"' "
        +(q.pro?"AND t.Teleports=0 ":"")+"AND NOT EXISTS (SELECT 1 FROM Bans b WHERE b.SteamID64=t.SteamID64 "
        "AND (b.ExpiresAt IS NULL OR b.ExpiresAt>CURRENT_TIMESTAMP)) "
        "AND NOT EXISTS (SELECT 1 FROM Times pb WHERE pb.SteamID64=t.SteamID64 AND pb.MapCourseID=t.MapCourseID "
        "AND pb.ModeID=t.ModeID AND pb.StyleIDFlags=t.StyleIDFlags "
        +(q.pro?"AND pb.Teleports=0 ":"")
        +"AND (pb.RunTime<t.RunTime OR (pb.RunTime=t.RunTime AND pb.ID<t.ID))) "
        "ORDER BY t.RunTime,t.ID LIMIT 1 OFFSET "+std::to_string(q.rank-1);
}
} // namespace line
