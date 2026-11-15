#pragma once
#include "route.h"
#include <atomic>
#include <string_view>

namespace line {
struct DecodeOptions {
    size_t maxFileBytes=128*1024*1024, maxDecodedBytes=256*1024*1024, maxTicks=2000000, maxSegments=250000;
    size_t maxLandings=50000;
    float spacing=8, maxStep=128, lineError=1.0f;
    float pathXYError=6,pathZError=3,maxPathLength=256,curveFlatness=3;
    bool smoothPath=true;
    bool hideFailedAttempts=true;
};
// Throws with a readable error on malformed, unsupported, mismatched or oversized data.
// By default, the drawing omits attempts bracketed by a recognized return teleport.
Route decodeReplay(std::string_view bytes,const Query& query,const Record& record,
                   const DecodeOptions& options={},const std::atomic<bool>* cancel=nullptr);
} // namespace line
