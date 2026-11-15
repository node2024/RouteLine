#pragma once
#include <cstdint>

namespace line {
// Only the selected replay window is allocated. Send its beams to authenticated
// subscribers, as shavit-line sends temporary beams directly to the selected client.
inline constexpr uint8_t AnnotationTeam=0;

inline bool annotationVisibleTo(uint64_t subscribers,int recipient,uint64_t authenticated) {
    return recipient>=0&&recipient<64&&(subscribers&authenticated&(uint64_t(1)<<recipient));
}

template<class Bits>
bool filterAnnotationTransmission(Bits* transmitted,Bits* always,Bits* outOfPVS,int index,
                                  uint64_t subscribers,int recipient,uint64_t authenticated,bool stop=false) {
    if(annotationVisibleTo(subscribers,recipient,authenticated)) {
        if(!transmitted) return false;
        transmitted->Set(index);
        if(stop&&outOfPVS) outOfPVS->Clear(index);
        return true;
    }
    if(transmitted) transmitted->Clear(index);
    if(always) always->Clear(index);
    if(outOfPVS) outOfPVS->Clear(index);
    return false;
}
} // namespace line
