#include "visibility.h"
#include <array>
#include <bitset>
#include <cassert>
#include <iostream>

namespace {
constexpr int EntityIndex=17;
struct Bits {
    std::bitset<64> bits;
    void Set(int index) {bits.set(index);}
    void Clear(int index) {bits.reset(index);}
    bool sent() const {return bits.test(EntityIndex);}
};
struct Recipient {Bits transmitted,always,outOfPVS;};
using Recipients=std::array<Recipient,4>;

void routeline(Recipients& clients,uint64_t subscribers,uint64_t authenticated) {
    const int slots[]={0,1,-1,63}; // Subscriber, another player, HLTV, high-bit subscriber.
    for(size_t i=0;i<clients.size();++i) {
        auto& c=clients[i];
        line::filterAnnotationTransmission(&c.transmitted,&c.always,&c.outOfPVS,EntityIndex,subscribers,slots[i],authenticated);
    }
}
void cs2kzQuiet(Recipients& clients,uint8_t team) {
    // KZ::quiet::OnCheckTransmit: unknown team-5 particle handles are hidden from
    // human clients. RouteLine's handles are not registered as CS2KZ measure/beam/zone handles.
    if(team==5) for(auto& c:clients) c.transmitted.Clear(EntityIndex);
}
void privateToOwner(const Recipients& clients) {
    assert(clients[0].transmitted.sent());
    for(size_t i=1;i<clients.size();++i) {
        assert(!clients[i].transmitted.sent());assert(!clients[i].always.sent());assert(!clients[i].outOfPVS.sent());
    }
}
}
int main() {
    Recipients legacy{};legacy[0].transmitted.Set(EntityIndex);routeline(legacy,1,1);cs2kzQuiet(legacy,5);
    assert(!legacy[0].transmitted.sent()); // Reproduce the reported 0.2.0 disappearance.
    for(bool quietLast:{false,true}) {
        Recipients clients{};
        for(auto& c:clients) {c.transmitted.Set(EntityIndex);c.always.Set(EntityIndex);c.outOfPVS.Set(EntityIndex);}
        if(!quietLast) cs2kzQuiet(clients,line::AnnotationTeam);
        routeline(clients,1,1);
        if(quietLast) cs2kzQuiet(clients,line::AnnotationTeam);
        privateToOwner(clients);
        routeline(clients,1,0); // Disconnect/reused slot must lose its old display.
        for(const auto& c:clients) assert(!c.transmitted.sent()&&!c.always.sent()&&!c.outOfPVS.sent());
    }
    Recipients shared{};for(auto& c:shared) c.transmitted.Set(EntityIndex);
    const uint64_t mask=1|(uint64_t(1)<<63);
    routeline(shared,mask,mask);assert(shared[0].transmitted.sent()&&shared[3].transmitted.sent());
    assert(!shared[1].transmitted.sent()&&!shared[2].transmitted.sent());
    routeline(shared,mask,uint64_t(1)<<63);assert(!shared[0].transmitted.sent()&&shared[3].transmitted.sent());
    routeline(shared,0,mask);for(const auto& c:shared) assert(!c.transmitted.sent()); // Retired entities.
    assert(!line::annotationVisibleTo(mask,64,mask));assert(!line::annotationVisibleTo(mask,-1,mask));
    Bits main;line::filterAnnotationTransmission(&main,static_cast<Bits*>(nullptr),static_cast<Bits*>(nullptr),EntityIndex,1,0,1);
    assert(main.sent()); // Selected beams are delivered directly to their subscriber.
    main.Set(EntityIndex);
    line::filterAnnotationTransmission(static_cast<Bits*>(nullptr),&main,&main,EntityIndex,1,0,1);
    assert(main.sent());
    line::filterAnnotationTransmission(static_cast<Bits*>(nullptr),&main,&main,EntityIndex,0,0,1);
    assert(!main.sent()); // Privacy also applies if only an auxiliary transmit mask exists.
    Recipient stopped{};
    stopped.outOfPVS.Set(EntityIndex);
    assert(line::filterAnnotationTransmission(&stopped.transmitted,&stopped.always,&stopped.outOfPVS,EntityIndex,1,0,1,true));
    assert(stopped.transmitted.sent()&&!stopped.outOfPVS.sent()); // Send the complete stop state to the former owner.
    assert(!line::filterAnnotationTransmission(&stopped.transmitted,&stopped.always,&stopped.outOfPVS,EntityIndex,1,1,3,true));
    assert(!stopped.transmitted.sent());
    std::cout<<"Selected beam delivery, private subscriber masks, high bit, HLTV and stale owners: passed\n";
}
