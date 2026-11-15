#pragma once
#include "config.h"
#include <array>

namespace line {
enum class PrimitiveKind { Route,Crouch,Landing,Jump,PerfectJump };
inline bool isRoutePath(PrimitiveKind kind) {return kind==PrimitiveKind::Route||kind==PrimitiveKind::Crouch;}
inline bool isJumpMarker(PrimitiveKind kind) {return kind==PrimitiveKind::Jump||kind==PrimitiveKind::PerfectJump;}
inline constexpr size_t FlatMarkerBatchSize=4;
// The first contact is the entity origin. Keep its other sprites close to it.
inline constexpr float FlatMarkerBatchRadius=256;
struct Primitive {
    uint64_t id;
    Segment segment;
    PrimitiveKind kind;
    uint32_t firstFrame,lastFrame;
    std::vector<Vec3> ropeNodes={};
    std::vector<bool> nodeCrouched={}; // Per-node tint, present only for mixed-color paths.
    std::vector<Vec3> flatMarkerOrigins={};
    bool ladderContact=false;
};
struct FrameWindow {
    size_t begin=0,end=0;
    bool all=true;
    bool operator==(const FrameWindow&) const = default;
};
inline FrameWindow drawingWindowAtFrame(const Route& route,size_t nearest,const Settings& settings,FrameWindow current={}) {
    if(!settings.follow||route.frames.empty()) return {0,route.frames.size(),true};
    size_t first=0,end=route.frames.size();
    if(settings.separateSections) {
        const auto section=route.frames[nearest].section;
        first=std::lower_bound(route.frames.begin(),route.frames.end(),section,
            [](const ReplayFrame& f,uint32_t s){return f.section<s;})-route.frames.begin();
        end=std::upper_bound(route.frames.begin(),route.frames.end(),section,
            [](uint32_t s,const ReplayFrame& f){return s<f.section;})-route.frames.begin();
    }
    const auto count=std::min(settings.replayFrames,end-first);
    if(count==end-first) return {first,end,false};
    const auto lastBegin=end-count;
    if(settings.windowOverlap&&settings.windowOverlap<count) {
        const auto step=count-settings.windowOverlap;
        const auto begin=std::min(first+(nearest-first)/step*step,lastBegin);
        // A small backward movement at the handoff should not recreate the last window.
        const auto rewindMargin=std::min(size_t(32),settings.windowOverlap/4);
        if(!current.all&&current.begin>=first&&current.end<=end&&begin<current.begin&&nearest+rewindMargin>=current.begin) return current;
        return {begin,begin+count,false};
    }
    const auto half=count/2;
    const auto begin=std::min(nearest-first>half?nearest-half:first,lastBegin);
    return {begin,begin+count,false};
}
inline FrameWindow drawingWindow(const Route& route,Vec3 position,const Settings& settings,FrameWindow current={}) {
    return drawingWindowAtFrame(route,settings.follow?route.nearestFrame(position):0,settings,current);
}
inline std::array<Segment,4> landingOutline(Vec3 position,float size) {
    const float half=size*0.5f;
    const Vec3 a=position+Vec3{-half,-half,0},b=position+Vec3{half,-half,0};
    const Vec3 c=position+Vec3{half,half,0},d=position+Vec3{-half,half,0};
    return {{{a,b},{b,c},{c,d},{d,a}}};
}
// Keep a long packed path inside a Follow page at its usual handoff points.
inline size_t nextFollowBoundary(const Route& route,size_t frame,const Settings& settings) {
    if(frame>=route.frames.size()||!settings.windowOverlap||settings.windowOverlap>=settings.replayFrames)
        return SIZE_MAX;
    size_t first=0,end=route.frames.size();
    if(settings.separateSections) {
        const auto section=route.frames[frame].section;
        first=std::lower_bound(route.frames.begin(),route.frames.begin()+frame,section,
            [](const ReplayFrame& f,uint32_t s){return f.section<s;})-route.frames.begin();
        end=std::upper_bound(route.frames.begin()+frame,route.frames.end(),section,
            [](uint32_t s,const ReplayFrame& f){return s<f.section;})-route.frames.begin();
    }
    if(end-first<=settings.replayFrames) return end;
    const auto step=settings.replayFrames-settings.windowOverlap;
    auto nextAfter=[&](size_t base) {return frame<base?base:base+((frame-base)/step+1)*step;};
    auto boundary=std::min({end,nextAfter(first),nextAfter(first+settings.replayFrames)});
    const auto lastStart=end-settings.replayFrames;
    return lastStart>frame?std::min(boundary,lastStart):boundary;
}
inline std::vector<Primitive> buildWorkshopDrawing(const Route& route,const Settings& settings) {
    std::vector<Primitive> result;
    const auto maxFrameSpan=std::min(settings.pathBatchFrames,settings.replayFrames/4);
    const size_t maxNodes=settings.packedRoute?65:17;
    const float maxLength=std::min(settings.pathBatchLength,settings.routeParticleSpacing*(settings.packedRoute?432:480));
    // Alpha is per entity. Preserve a configured alpha difference by splitting those paths.
    const bool mixColors=settings.packedRoute&&settings.colors[3]==settings.colors[11];
    if(settings.detail!=DisplayDetail::Landings) {
        for(size_t i=0;i<route.path.size();) {
            const auto& first=route.path[i];
            float length=std::sqrt(distance2(first.start,first.end));
            if(length<=0.01f) {++i;continue;}
            Primitive chunk{uint64_t(i),first,first.crouched?PrimitiveKind::Crouch:PrimitiveKind::Route,
                first.firstFrame,first.lastFrame,{first.start,first.end}};
            const auto boundary=settings.packedRoute?nextFollowBoundary(route,first.firstFrame,settings):SIZE_MAX;
            ++i;
            while(i<route.path.size()) {
                const auto& next=route.path[i];
                const float nextLength=std::sqrt(distance2(next.start,next.end));
                const bool previousCrouched=chunk.nodeCrouched.empty()?first.crouched:chunk.nodeCrouched.back();
                const bool changesColor=previousCrouched!=next.crouched;
                const size_t addedNodes=changesColor?2:1;
                if(nextLength<=0.01f||next.section!=first.section||(!mixColors&&changesColor)
                   ||chunk.ropeNodes.size()+addedNodes>maxNodes||next.lastFrame>=boundary
                   ||distance2(chunk.ropeNodes.back(),next.start)>0.0001f
                   ||length+nextLength>maxLength||next.lastFrame-chunk.firstFrame>maxFrameSpan) break;
                if(changesColor) {
                    const auto corner=chunk.ropeNodes.back();
                    const auto incoming=corner-chunk.ropeNodes[chunk.ropeNodes.size()-2];
                    const float cut=std::min(0.25f,0.25f/std::sqrt(incoming.dot(incoming)));
                    const auto before=corner-incoming*cut;
                    if(distance2(before,corner)<0.000001f) break;
                    if(chunk.nodeCrouched.empty()) chunk.nodeCrouched.assign(chunk.ropeNodes.size(),previousCrouched);
                    chunk.ropeNodes.back()=before;
                    chunk.ropeNodes.push_back(corner);
                    chunk.nodeCrouched.push_back(next.crouched);
                }
                chunk.ropeNodes.push_back(next.end);
                if(!chunk.nodeCrouched.empty()) chunk.nodeCrouched.push_back(next.crouched);
                chunk.lastFrame=next.lastFrame;
                length+=nextLength;
                ++i;
            }
            result.push_back(std::move(chunk));
        }
    }
    if(settings.landingMarkers) {
        std::array<size_t,4> batches{SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
        std::array<size_t,4> boundaries{};
        const bool flat=settings.useFlatMarkers();
        const bool canBatch=flat&&!settings.meshMarkers&&(settings.windowOverlap||route.frames.empty());
        for(size_t i=0;i<route.landings.size();++i) {
            const auto& landing=route.landings[i];
            const size_t batch=(landing.ladder?2:0)+(landing.crouched?1:0);
            const auto previous=batches[batch];
            if(canBatch&&previous!=SIZE_MAX&&landing.frameIndex<boundaries[batch]
               &&result[previous].flatMarkerOrigins.size()<FlatMarkerBatchSize
               &&distance2(landing.position,result[previous].flatMarkerOrigins.front())<=FlatMarkerBatchRadius*FlatMarkerBatchRadius) {
                result[previous].flatMarkerOrigins.push_back(landing.position);
                result[previous].lastFrame=landing.frameIndex;
                continue;
            }
            Primitive marker{(uint64_t(1)<<63)|(uint64_t(i)<<2),
                {landing.position,landing.position,landing.crouched},PrimitiveKind::Landing,landing.frameIndex,landing.frameIndex};
            marker.ladderContact=landing.ladder;
            if(flat) marker.flatMarkerOrigins.push_back(landing.position);
            if(canBatch) {
                batches[batch]=result.size();
                boundaries[batch]=nextFollowBoundary(route,landing.frameIndex,settings);
            }
            result.push_back(std::move(marker));
        }
    }
    if(settings.useTakeoffMarkers()) {
        std::array<size_t,2> batches{SIZE_MAX,SIZE_MAX};
        std::array<size_t,2> boundaries{};
        const bool canBatch=!settings.meshMarkers&&(settings.windowOverlap||route.frames.empty());
        for(size_t i=0;i<route.jumps.size();++i) {
            const auto& jump=route.jumps[i];
            const size_t batch=jump.perfect;
            const auto previous=batches[batch];
            if(canBatch&&previous!=SIZE_MAX&&jump.frameIndex<boundaries[batch]
               &&result[previous].segment.section==jump.section
               &&result[previous].flatMarkerOrigins.size()<FlatMarkerBatchSize
               &&distance2(jump.position,result[previous].flatMarkerOrigins.front())<=FlatMarkerBatchRadius*FlatMarkerBatchRadius) {
                result[previous].flatMarkerOrigins.push_back(jump.position);
                result[previous].lastFrame=jump.frameIndex;
                continue;
            }
            if(canBatch) {
                batches[batch]=result.size();
                boundaries[batch]=nextFollowBoundary(route,jump.frameIndex,settings);
            }
            result.push_back({(uint64_t(3)<<62)|(uint64_t(i)<<2),{jump.position,jump.position,false,0,0,jump.section},
                jump.perfect?PrimitiveKind::PerfectJump:PrimitiveKind::Jump,jump.frameIndex,jump.frameIndex,{}, {}, {jump.position}});
        }
    }
    return result;
}
// The route follows recorded path samples. Ground contacts are marker anchors,
// never the endpoints of a shortcut through a turn or across an obstacle.
inline std::vector<Primitive> buildDrawing(const Route& route,const Settings& settings) {
    return buildWorkshopDrawing(route,settings);
}
inline size_t drawingSize(const Route& route,const Settings& settings) {return buildDrawing(route,settings).size();}
inline std::vector<size_t> selectDrawing(const std::vector<Primitive>& drawing,const FrameWindow& window) {
    std::vector<size_t> result;
    for(size_t i=0;i<drawing.size();++i) if(window.all||(drawing[i].firstFrame>=window.begin&&drawing[i].lastFrame<window.end)) result.push_back(i);
    return result;
}
inline bool routePathInRange(const Primitive& path,Vec3 position,float radius) {
    const float radiusSquared=radius*radius;
    for(size_t i=1;i<path.ropeNodes.size();++i) {
        if(distance2(position,Segment{path.ropeNodes[i-1],path.ropeNodes[i]})<=radiusSquared) return true;
    }
    return false;
}
inline bool markerInRange(const Primitive& marker,Vec3 position,float radius) {
    const float radiusSquared=radius*radius;
    if(marker.flatMarkerOrigins.empty()) return distance2(position,marker.segment.start)<=radiusSquared;
    return std::any_of(marker.flatMarkerOrigins.begin(),marker.flatMarkerOrigins.end(),
        [&](Vec3 origin){return distance2(position,origin)<=radiusSquared;});
}
inline float followPreviewDistance(float radius) {return radius+std::min(256.0f,radius/4);}
inline int pathOverlapDirection(const Primitive& path,Vec3 point,Vec3 direction) {
    constexpr float overlapRadius=24;
    int match=0;
    for(size_t i=1;i<path.ropeNodes.size();++i) {
        const auto delta=path.ropeNodes[i]-path.ropeNodes[i-1];
        const float projection=direction.dot(delta);
        // Accept both travel directions, but ignore an isolated crossing.
        if(projection*projection>delta.dot(delta)*0.5f
           &&distance2(point,Segment{path.ropeNodes[i-1],path.ropeNodes[i]})<=overlapRadius*overlapRadius) {
            if(projection>0) return 1;
            match=-1;
        }
    }
    return match;
}
enum class FollowOverlap { None, Parallel, SameDirection };
template<class Along> inline FollowOverlap previewOverlap(const Primitive& path,Along along) {
    constexpr float step=16;
    constexpr size_t maxSamples=32;
    float offset=step/2;
    size_t samples=0,matched=0,forward=0;
    bool parallel=false;
    for(size_t i=1;i<path.ropeNodes.size()&&samples<maxSamples;++i) {
        const auto start=path.ropeNodes[i-1],delta=path.ropeNodes[i]-start;
        const float length=std::sqrt(delta.dot(delta));
        if(length==0) continue;
        const auto direction=delta*(1/length);
        for(;offset<length&&samples<maxSamples;offset+=step,++samples) {
            const int match=along(start+direction*offset,direction);
            matched=match?matched+1:0;
            forward=match>0?forward+1:0;
            if(forward==3) return FollowOverlap::SameDirection;
            parallel=parallel||matched>=3;
        }
        offset-=length;
    }
    return parallel?FollowOverlap::Parallel:FollowOverlap::None;
}
struct FollowContinuation {size_t index;FollowOverlap overlap;};
// Find the first missing batch ahead and check whether it retraces the visible path.
inline std::optional<FollowContinuation> followContinuation(const std::vector<Primitive>& drawing,
    const std::vector<size_t>& selected,size_t nearest,Vec3 position,const Settings& settings) {
    const size_t lead=std::min(size_t(256),settings.replayFrames/4);
    const auto end=std::partition_point(selected.begin(),selected.end(),
        [&](size_t i){return isRoutePath(drawing[i].kind);});
    auto anchor=std::lower_bound(selected.begin(),end,nearest,
        [&](size_t i,size_t frame){return drawing[i].lastFrame<frame;});
    if(anchor==end||drawing[*anchor].firstFrame>nearest) {
        if(anchor==selected.begin()) return std::nullopt;
        --anchor;
    }
    if(drawing[*anchor].lastFrame+lead<nearest) return std::nullopt;
    size_t next=*anchor+1;
    for(++anchor;anchor!=end&&*anchor==next;++anchor) ++next;
    if(next>=drawing.size()||!isRoutePath(drawing[next].kind)) return std::nullopt;
    const auto& path=drawing[next];
    if(path.firstFrame>nearest+lead||path.lastFrame<nearest||path.lastFrame>=nearest+settings.replayFrames)
        return std::nullopt;
    if(!routePathInRange(path,position,followPreviewDistance(settings.routeDistance))) return std::nullopt;
    if(path.segment.section==drawing[next-1].segment.section) return FollowContinuation{next,FollowOverlap::None};
    const auto overlap=previewOverlap(path,[&](Vec3 point,Vec3 direction) {
        int match=0;
        for(auto it=selected.begin();it!=end;++it) {
            const auto& visible=drawing[*it];
            if(visible.lastFrame>path.firstFrame||visible.segment.section==path.segment.section) continue;
            const int candidate=pathOverlapDirection(visible,point,direction);
            if(candidate>0) return candidate;
            if(candidate<0) match=candidate;
        }
        return match;
    });
    return FollowContinuation{next,overlap};
}
inline std::optional<size_t> followPreviewMarker(const std::vector<Primitive>& drawing,const std::vector<size_t>& selected,
    const Primitive& path,const FrameWindow& window,Vec3 position,float radius) {
    const auto markers=std::partition_point(drawing.begin(),drawing.end(),
        [](const Primitive& p){return isRoutePath(p.kind);});
    const auto end=std::partition_point(markers,drawing.end(),
        [](const Primitive& p){return p.kind==PrimitiveKind::Landing;});
    auto marker=std::lower_bound(markers,end,window.begin,
        [](const Primitive& p,size_t frame){return p.firstFrame<frame;});
    for(;marker!=end&&marker->firstFrame<=path.lastFrame;++marker) {
        if(marker->lastFrame>=window.end) continue;
        const bool onPath=marker->firstFrame>=path.firstFrame
            ||(marker->lastFrame>=path.firstFrame&&marker->lastFrame<=path.lastFrame);
        const auto index=size_t(marker-drawing.begin());
        if(onPath&&!std::binary_search(selected.begin(),selected.end(),index)&&markerInRange(*marker,position,radius))
            return index;
    }
    return std::nullopt;
}
inline const uint8_t* primitiveColor(PrimitiveKind kind,const Settings& settings) {
    if(kind==PrimitiveKind::Landing) return settings.colors.data()+4;
    if(kind==PrimitiveKind::Crouch) return settings.colors.data()+8;
    return settings.colors.data();
}
inline const uint8_t* primitiveColor(const Primitive& primitive,const Settings& settings) {
    if(primitive.kind==PrimitiveKind::Landing&&primitive.segment.crouched)
        return primitiveColor(PrimitiveKind::Crouch,settings);
    return primitiveColor(primitive.kind,settings);
}
} // namespace line
