#pragma once
#include "route.h"
#include <stdexcept>

namespace line {
// Douglas-Peucker with separate horizontal/vertical error bounds. Every retained
// corner is an actual replay sample. Protected movement-state changes are never
// shortcut, and separate teleport sections are never joined.
inline std::vector<Segment> simplifyPath(const std::vector<PathSample>& input,float xyError,float zError,float maxLength,
                                        size_t limit,const std::atomic<bool>* cancel=nullptr) {
    std::vector<Segment> result;
    if(!std::isfinite(xyError)||xyError<=0||!std::isfinite(zError)||zError<=0||!std::isfinite(maxLength)||maxLength<=0)
        throw std::runtime_error("Invalid path simplification settings.");
    if(input.size()<2) return result;
    std::vector<uint8_t> keep(input.size(),0);
    auto checkCancel=[&] {if(cancel&&cancel->load()) throw std::runtime_error("Replay loading cancelled.");};
    auto simplify=[&](size_t first,size_t last) {
        std::vector<std::pair<size_t,size_t>> stack{{first,last}};
        while(!stack.empty()) {
            checkCancel();
            const auto [a,b]=stack.back();stack.pop_back();
            if(b<=a+1) continue;
            const auto start=input[a].position,delta=input[b].position-start;
            const float lengthXY=delta.x*delta.x+delta.y*delta.y,length3D=delta.dot(delta);
            float worst=1;size_t split=0;
            for(size_t i=a+1;i<b;++i) {
                if((i&4095)==0) checkCancel();
                const auto relative=input[i].position-start;
                const float t=std::clamp(lengthXY>0.0001f?(relative.x*delta.x+relative.y*delta.y)/lengthXY:
                    length3D>0.0001f?relative.dot(delta)/length3D:0,0.0f,1.0f);
                const auto error=input[i].position-(start+delta*t);
                const float score=std::max((error.x*error.x+error.y*error.y)/(xyError*xyError),error.z*error.z/(zError*zError));
                if(score>worst) {worst=score;split=i;}
            }
            if(!split&&length3D>maxLength*maxLength) split=a+(b-a)/2;
            if(split) {keep[split]=1;stack.emplace_back(a,split);stack.emplace_back(split,b);}
        }
    };
    size_t first=0;
    while(first<input.size()) {
        size_t last=first;
        while(last+1<input.size()&&input[last+1].section==input[first].section) ++last;
        keep[first]=keep[last]=1;size_t anchor=first;
        for(size_t i=first+1;i<=last;++i) if(input[i].anchor||i==last) {
            keep[i]=1;simplify(anchor,i);anchor=i;
        }
        size_t previous=first;
        for(size_t i=first+1;i<=last;++i) if(keep[i]) {
            if(distance2(input[previous].position,input[i].position)>0.0001f) {
                const auto start=input[previous].position,delta=input[i].position-start;
                const size_t parts=std::max(size_t(1),size_t(std::ceil(std::sqrt(delta.dot(delta))/maxLength)));
                if(result.size()>limit||parts>limit-result.size()) throw std::runtime_error("Simplified path exceeds the configured segment limit.");
                for(size_t part=0;part<parts;++part) result.push_back({start+delta*(float(part)/parts),start+delta*(float(part+1)/parts),
                    input[previous].crouched,input[previous].frame,input[i].frame,input[i].section});
            }
            previous=i;
        }
        first=last+1;
    }
    return result;
}
} // namespace line
