#pragma once
#include "path.h"
#include <array>

namespace line {
struct Cubic {
    Vec3 p0,p1,p2,p3;
    Vec3 at(float t) const {const float s=1-t;return p0*(s*s*s)+p1*(3*s*s*t)+p2*(3*s*t*t)+p3*(t*t*t);}
    Vec3 derivative(float t) const {const float s=1-t;return (p1-p0)*(3*s*s)+(p2-p1)*(6*s*t)+(p3-p2)*(3*t*t);}
    Cubic portion(float a,float b) const {const auto start=at(a),end=at(b);return {start,start+derivative(a)*((b-a)/3),end-derivative(b)*((b-a)/3),end};}
};
inline Vec3 unitVector(Vec3 p) {const float length=std::sqrt(p.dot(p));return length>0.0001f?p*(1/length):Vec3{};}
inline float scaledDistance2(Vec3 point,Vec3 start,Vec3 end,float xy,float z) {
    const Vec3 p{point.x/xy,point.y/xy,point.z/z},a{start.x/xy,start.y/xy,start.z/z},b{end.x/xy,end.y/xy,end.z/z};
    return distance2(p,Segment{a,b});
}
// Fit cubic Beziers, then draw them with a small, adaptively chosen number of
// native straight beams. Color changes divide a curve without changing its shape.
// Curved spans require every source interval's restricted control hull to stay
// inside an anisotropic capsule (a convex set). Near-linear spans use a bounded
// chord instead. A final merge checks the complete source interval again.
inline std::vector<Segment> smoothPath(const std::vector<PathSample>& input,float xyError,float zError,float flatness,
                                      float maxLength,size_t limit,const std::atomic<bool>* cancel=nullptr,size_t* curveCount=nullptr) {
    if(!std::isfinite(xyError)||xyError<=0||!std::isfinite(zError)||zError<=0||!std::isfinite(flatness)||flatness<=0||!std::isfinite(maxLength)||maxLength<=0)
        throw std::runtime_error("Invalid curve settings.");
    std::vector<Segment> result;size_t accepted=0;
    if(input.size()<2) {if(curveCount) *curveCount=0;return result;}
    auto checkCancel=[&] {if(cancel&&cancel->load()) throw std::runtime_error("Replay loading cancelled.");};
    auto tangent=[&](size_t i,size_t first,size_t last) {
        size_t before=i,after=i;
        while(before>first&&distance2(input[i].position,input[before].position)<24*24) --before;
        while(after<last&&distance2(input[i].position,input[after].position)<24*24) ++after;
        return unitVector(input[after].position-input[before].position);
    };
    auto process=[&](size_t begin,size_t end,size_t sectionFirst,size_t sectionLast) {
        std::vector<std::pair<size_t,size_t>> pending{{begin,end}};
        while(!pending.empty()) {
            checkCancel();const auto [a,b]=pending.back();pending.pop_back();if(b<=a) continue;
            std::vector<float> u(b-a+1,0);
            for(size_t i=a+1;i<=b;++i) u[i-a]=u[i-a-1]+std::sqrt(distance2(input[i-1].position,input[i].position));
            const float arc=u.back();if(arc<0.01f) continue;
            if(arc>1024&&b>a+1) {const auto split=a+(b-a)/2;pending.emplace_back(split,b);pending.emplace_back(a,split);continue;}
            for(auto& t:u) t/=arc;
            const auto p0=input[a].position,p3=input[b].position;
            const auto startTangent=tangent(a,sectionFirst,sectionLast),endTangent=tangent(b,sectionFirst,sectionLast);
            double c00=0,c01=0,c11=0,x0=0,x1=0;
            for(size_t i=a;i<=b;++i) {
                const float t=u[i-a],s=1-t,b0=s*s*s,b1=3*s*s*t,b2=3*s*t*t,b3=t*t*t;
                const auto v0=startTangent*b1,v1=endTangent*(-b2),r=input[i].position-(p0*(b0+b1)+p3*(b2+b3));
                c00+=v0.dot(v0);c01+=v0.dot(v1);c11+=v1.dot(v1);x0+=v0.dot(r);x1+=v1.dot(r);
            }
            const double determinant=c00*c11-c01*c01;
            float alpha=arc/3,beta=arc/3;
            if(std::abs(determinant)>1e-10) {alpha=float((x0*c11-x1*c01)/determinant);beta=float((c00*x1-c01*x0)/determinant);}
            if(!std::isfinite(alpha)||alpha<=0||alpha>arc*2) alpha=arc/3;
            if(!std::isfinite(beta)||beta<=0||beta>arc*2) beta=arc/3;
            Cubic curve{p0,p0+startTangent*alpha,p3-endTangent*beta,p3};
            bool straight=true;
            for(size_t i=a;i<=b;++i) {
                const float z=input[i].grounded||input[i].curveAnchor?std::min(zError,0.5f):zError;
                if(scaledDistance2(input[i].position,p0,p3,xyError,z)>1) {straight=false;break;}
            }
            if(straight||b==a+1) curve={p0,p0+(p3-p0)*(1.0f/3),p0+(p3-p0)*(2.0f/3),p3};
            float worst=1;size_t split=0;
            for(size_t i=a;!straight&&i<b;++i) {
                if((i&4095)==0) checkCancel();
                const auto restricted=curve.portion(u[i-a],u[i+1-a]);
                const bool guarded=input[i].grounded||input[i+1].grounded||input[i].curveAnchor||input[i+1].curveAnchor;
                const float z=guarded?std::min(zError,0.5f):zError;
                for(const auto& p:std::array<Vec3,4>{restricted.p0,restricted.p1,restricted.p2,restricted.p3}) {
                    const float score=scaledDistance2(p,input[i].position,input[i+1].position,xyError,z);
                    if(b>a+1&&score>worst+0.0001f) {worst=score;split=std::clamp(i+1,a+1,b-1);}
                }
            }
            if(split&&b>a+1) {pending.emplace_back(split,b);pending.emplace_back(a,split);continue;}
            ++accepted;
            bool allGrounded=true;for(size_t i=a;i<=b;++i) allGrounded&=input[i].grounded;
            auto frameAt=[&](float t,bool upper) {
                auto it=std::lower_bound(u.begin(),u.end(),t);
                size_t index=size_t(it-u.begin());
                if(index>=u.size()) index=u.size()-1;
                if(!upper&&index&&u[index]>t+0.000001f) --index;
                return input[a+index].frame;
            };
            auto draw=[&](float from,float to,bool crouched) {
                struct Piece {float a,b;int depth;};std::vector<Piece> pieces{{from,to,0}};
                while(!pieces.empty()) {
                    checkCancel();const auto piece=pieces.back();pieces.pop_back();const auto c=curve.portion(piece.a,piece.b);
                    const bool nearAnchor=allGrounded||(input[a].curveAnchor&&piece.a*arc<16)||(input[b].curveAnchor&&(1-piece.b)*arc<16);
                    const float z=nearAnchor?std::min(flatness,0.5f):flatness;
                    const bool fits=scaledDistance2(c.p1,c.p0,c.p3,flatness,z)<=1&&scaledDistance2(c.p2,c.p0,c.p3,flatness,z)<=1;
                    if(piece.depth<16&&(!fits||distance2(c.p0,c.p3)>maxLength*maxLength)) {
                        const float mid=(piece.a+piece.b)*0.5f;
                        pieces.push_back({mid,piece.b,piece.depth+1});pieces.push_back({piece.a,mid,piece.depth+1});continue;
                    }
                    if(distance2(c.p0,c.p3)>0.0001f) {
                        if(result.size()>=limit) throw std::runtime_error("Smooth path exceeds the configured segment limit.");
                        result.push_back({c.p0,c.p3,crouched,frameAt(piece.a,false),frameAt(piece.b,true),input[a].section});
                    }
                }
            };
            float from=0;bool crouched=input[a].crouched;
            for(size_t i=a+1;i<=b;++i) if(input[i].crouched!=crouched) {
                if(u[i-a]>from+0.000001f) draw(from,u[i-a],crouched);
                from=u[i-a];crouched=input[i].crouched;
            }
            if(from<1) draw(from,1,crouched);
        }
    };
    for(size_t first=0;first<input.size();) {
        size_t last=first;while(last+1<input.size()&&input[last+1].section==input[first].section) ++last;
        size_t anchor=first;
        for(size_t i=first+1;i<=last;++i) if(input[i].curveAnchor||i==last) {process(anchor,i,first,last);anchor=i;}
        first=last+1;
    }
    // Avoid paying for many tiny Bezier pieces at join points. Coalesce them only
    // when the displayed shape stays close, the original samples satisfy a fixed
    // total error budget, and all contact/apex anchors remain on the chord.
    std::vector<Segment> compact;compact.reserve(result.size());
    for(size_t first=0;first<result.size();) {
        size_t last=first;
        while(last+1<result.size()&&last-first<63&&result[last+1].section==result[first].section
              &&result[last+1].crouched==result[first].crouched&&distance2(result[last].end,result[last+1].start)<0.0001f) {
            checkCancel();const Segment candidate{result[first].start,result[last+1].end,result[first].crouched,
                result[first].firstFrame,result[last+1].lastFrame,result[first].section};
            if(distance2(candidate.start,candidate.end)>maxLength*maxLength) break;
            bool fits=true,grounded=false;
            const auto begin=std::lower_bound(input.begin(),input.end(),candidate.firstFrame,[](const PathSample& p,uint32_t f){return p.frame<f;});
            for(auto it=begin;it!=input.end()&&it->frame<=candidate.lastFrame;++it) if(it->section==candidate.section) {
                grounded|=it->grounded;
                if(it->curveAnchor&&distance2(it->position,candidate)>0.0001f) {fits=false;break;}
                const float z=it->grounded||it->curveAnchor?1.0f:zError+flatness;
                if(scaledDistance2(it->position,candidate.start,candidate.end,xyError+flatness,z)>1.0001f) {fits=false;break;}
            }
            for(size_t i=first;fits&&i<=last;++i)
                if(scaledDistance2(result[i].end,candidate.start,candidate.end,flatness,grounded?std::min(flatness,1.0f):flatness)>1) fits=false;
            if(!fits) break;
            ++last;
        }
        compact.push_back({result[first].start,result[last].end,result[first].crouched,result[first].firstFrame,result[last].lastFrame,result[first].section});
        first=last+1;
    }
    if(curveCount) *curveCount=accepted;
    return compact;
}
} // namespace line
