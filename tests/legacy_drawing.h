#pragma once
#include "drawing.h"
namespace line {
inline std::vector<Primitive> buildLegacyDrawing(const Route& route,const Settings& settings) {
    std::vector<Primitive> result;
    const auto pathCount=settings.detail==DisplayDetail::Landings?0:route.path.size();
    const bool outlineMarkers=settings.detail==DisplayDetail::Full;
    result.reserve(pathCount+(settings.landingMarkers?route.landings.size()*(outlineMarkers?4:1):0));
    for(size_t i=0;i<pathCount;++i) {
        const auto& segment=route.path[i];
        if(distance2(segment.start,segment.end)>0.0001f)
            result.push_back({uint64_t(i),segment,segment.crouched?PrimitiveKind::Crouch:PrimitiveKind::Route,segment.firstFrame,segment.lastFrame});
    }
    if(settings.landingMarkers) for(size_t i=0;i<route.landings.size();++i) {
        const auto& contact=route.landings[i];
        const auto id=(uint64_t(1)<<63)|(uint64_t(i)<<2);
        if(outlineMarkers) {
            const auto outline=landingOutline(contact.position,settings.landingSize);
            for(size_t edge=0;edge<4;++edge) result.push_back({id|edge,outline[edge],PrimitiveKind::Landing,contact.frameIndex,contact.frameIndex});
        } else {
            const Vec3 half{std::min(4.0f,settings.landingSize*0.5f),0,0};
            const Segment dash{contact.position-half,contact.position+half};
            result.push_back({id,dash,PrimitiveKind::Landing,contact.frameIndex,contact.frameIndex});
        }
    }
    return result;
}
}
