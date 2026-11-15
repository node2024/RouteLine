#pragma once
#include "display.h"
#include "server_rtti.h"
#include "visibility.h"
#include "workshop_geometry.h"
#include <bspflags.h>
#include <gametrace.h>
#include <schemasystem/schemasystem.h>
#include <entity2/entityinstance.h>
#include <entity2/entitykeyvalues.h>
#include <entity2/variant.h>
#include <gameentitysystem.h>
#include <link.h>
#include <map>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>

namespace line {
// Unique executable signature matches; patterns are supplied in gamedata.json.
inline void* resolveSignature(const std::string& pattern) {
    std::istringstream stream(pattern);std::string token;std::vector<int> bytes;
    while(stream>>token) {
        if(token=="?"||token=="??") bytes.push_back(-1);
        else {size_t used=0;int b=std::stoi(token,&used,16);if(used!=2||b<0||b>255) throw std::runtime_error("Invalid signature byte.");bytes.push_back(b);}
    }
    if(bytes.size()<8) throw std::runtime_error("Signature is too short.");
    struct Search {const std::vector<int>& bytes;void* found=nullptr;int count=0;} search{bytes};
    dl_iterate_phdr([](dl_phdr_info* info,size_t,void* data) {
        auto& s=*static_cast<Search*>(data);const std::string name=info->dlpi_name;
        if(name.size()<12||name.substr(name.size()-12)!="libserver.so") return 0;
        for(int j=0;j<info->dlpi_phnum;++j) {
            const auto& p=info->dlpi_phdr[j];if(p.p_type!=PT_LOAD||!(p.p_flags&PF_X)||p.p_memsz<s.bytes.size()) continue;
            auto* start=reinterpret_cast<unsigned char*>(info->dlpi_addr+p.p_vaddr);
            for(size_t i=0;i<=p.p_memsz-s.bytes.size();++i) {
                size_t n=0;while(n<s.bytes.size()&&(s.bytes[n]<0||start[i+n]==s.bytes[n])) ++n;
                if(n==s.bytes.size()) {s.found=start+i;++s.count;}
            }
        }
        return 0;
    },&search);
    if(search.count!=1) throw std::runtime_error("Missing or ambiguous libserver signature. Update routeline/gamedata.json.");
    return search.found;
}
struct SchemaField {
    int offset=0;
    void init(ISchemaSystem* schema,const char* cls,const char* name) {
        auto* scope=schema->FindTypeScopeForModule("libserver.so");auto* c=scope?scope->FindDeclaredClass(cls).Get():nullptr;
        if(c) for(int i=0;i<c->m_nNumFields;++i) if(!std::strcmp(c->m_pFields[i].m_pName,name)) {
            offset=c->m_pFields[i].m_nSingleInheritanceOffset;if(offset>=0) return;
        }
        throw std::runtime_error(std::string("Missing schema field: ")+cls+"::"+name);
    }
    template<class T> T& get(void* entity) const {return *reinterpret_cast<T*>(static_cast<char*>(entity)+offset);}
};
class Renderer {
    using Clock=std::chrono::steady_clock;
    using Create=CEntityInstance*(*)(const char*,int);
    using Spawn=void(*)(CEntityInstance*,CEntityKeyValues*);
    using Remove=void(*)(CEntityInstance*);
    using AcceptInput=void(*)(CEntityInstance*,const char*,CEntityInstance*,CEntityInstance*,const variant_t*);
    using TraceLine=void(*)(void*,const Vector&,const Vector&,CTraceFilter*,CGameTrace*);
    using TraceShape=void(*)(void*,const Ray_t&,const Vector&,const Vector&,CTraceFilter*,CGameTrace*);
    void** navVTable_=nullptr;
    Create create_=nullptr;Spawn spawn_=nullptr;Remove remove_=nullptr;
    AcceptInput acceptInput_=nullptr;
    SchemaField team_;
    SchemaField ropeNodeCount_,ropeEffect_,ropeState_;
    SchemaField particleActive_,particleStopType_,particleEffect_;
    SchemaField modelBody_,modelScene_,modelState_,modelHandle_;
    bool workshopSupported_=false;
    bool flatMarkerSupported_=false;
    bool meshMarkerSupported_=false;
    struct Visual {CEntityHandle handle;bool workshop=false;bool flatMarker=false;};
    PersistentDisplays<Visual> displays_;
    size_t nextGroup_=0;
    struct Stopping {
        CEntityHandle handle;
        int slot;
        uint64_t steam;
        Clock::time_point fallback;
        std::optional<Clock::time_point> firstTransmit;
        std::optional<int> firstSnapshotTick;
        bool flatMarker;
    };
    std::vector<Stopping> stopping_;
    std::vector<CEntityHandle> retired_;
    CEntityInstance* entity(const Visual& visual) const {return GameEntitySystem()?GameEntitySystem()->GetEntityInstance(visual.handle):nullptr;}
    int effectState(const Visual& visual,CEntityInstance* e) const {
        if(!visual.workshop) return 1; // The mesh is visible for the lifetime of its entity.
        return visual.flatMarker?int(particleActive_.get<bool>(e)):ropeState_.get<int32_t>(e);
    }
    void removeNow(CEntityHandle handle,CEntityInstance* e) {
        retired_.push_back(handle);
        remove_(e);
    }
    void erase(const Visual& v,int slot=-1,uint64_t steam=0) {
        auto* e=entity(v);
        if(!e) return;
        if(v.workshop) {
            const auto activeState=effectState(v,e);
            const variant_t value;
            acceptInput_(e,"DestroyImmediately",nullptr,nullptr,&value);
            e=entity(v);
            if(!e) return;
            if(v.flatMarker) {
                particleActive_.get<bool>(e)=false;
                particleStopType_.get<int32_t>(e)=1; // STOP_IMMEDIATELY, also used by CS2KZ.
            }
            const auto stoppedState=effectState(v,e);
            if((v.flatMarker?stoppedState==0:stoppedState!=activeState)&&steam&&slot>=0) {
                // Keep the stopped entity in snapshots long enough for the former
                // owner to see the state change, even after the session is cleared.
                e->NetworkStateChanged(NetworkStateChangedData(true));
                const auto timeout=v.flatMarker?std::chrono::seconds(4):std::chrono::seconds(2);
                stopping_.push_back({v.handle,slot,steam,Clock::now()+timeout,{},{},v.flatMarker});
                return;
            }
        }
        removeNow(v.handle,e);
    }
    std::optional<GroundHit> probeGround(Vec3 position,bool footprint=false) {
        if(!navVTable_) return std::nullopt;
        const float range=footprint?1.5f:6.0f;
        const Vector start(position.x,position.y,position.z+range),end(position.x,position.y,position.z-range);
        constexpr uint64_t solid=CONTENTS_SOLID|CONTENTS_WINDOW|CONTENTS_PASS_BULLETS|CONTENTS_WORLD_GEOMETRY;
        CTraceFilter filter(footprint?solid|CONTENTS_PLAYER_CLIP:solid,COLLISION_GROUP_PLAYER_MOVEMENT,false);
        filter.EnableInteractsAsLayer(LAYER_INDEX_CONTENTS_PLAYER);
        filter.m_nObjectSetMask=RNQUERY_OBJECTS_STATIC|RNQUERY_OBJECTS_KEYFRAMED;
        CGameTrace trace;
        // The CNavPhysicsInterface wrapper has no instance state. Use its
        // filter overloads of Nav_TraceShape and Nav_TraceLine.
        if(footprint) {
            // Use the bottom of the player hull with Source 2's 1/32-unit clearance.
            constexpr float halfWidth=16.0f-1.0f/32;
            const Ray_t feet(Vector(-halfWidth,-halfWidth,0),Vector(halfWidth,halfWidth,0.25f));
            reinterpret_cast<TraceShape>(navVTable_[5])(&navVTable_,feet,start,end,&filter,&trace);
        } else {
            reinterpret_cast<TraceLine>(navVTable_[3])(&navVTable_,start,end,&filter,&trace);
        }
        if(!trace.DidHit()||trace.m_bStartInSolid||trace.m_vHitNormal.z<0.7f) return std::nullopt;
        const Vec3 point{trace.m_vEndPos.x,trace.m_vEndPos.y,trace.m_vEndPos.z};
        const Vec3 normal{trace.m_vHitNormal.x,trace.m_vHitNormal.y,trace.m_vHitNormal.z};
        if(!point.valid()||!normal.valid()||std::abs(point.z-position.z)>range) return std::nullopt;
        return GroundHit{point,normal};
    }
    Vec3 flatMarkerPosition(const Primitive& primitive,Vec3 position,const Settings& settings) {
        if(!isJumpMarker(primitive.kind))
            return workshopMarkerCenter(position,settings,[this](Vec3 p){return probeGround(p);},primitive.ladderContact);
        // Keep the takeoff XY and lift the X above coincident landing frames.
        if(const auto hit=probeGround(position)) position.z=hit->point.z+3.0f;
        else position.z+=settings.zOffset+0.5f;
        return position;
    }
    Visual createMeshMarker(const Primitive& primitive,const Settings& settings) {
        const bool jump=isJumpMarker(primitive.kind);
        const auto& asset=jump?WorkshopJumpMesh:WorkshopLandingMesh;
        const auto position=flatMarkerPosition(primitive,primitive.flatMarkerOrigins.front(),settings);
        const auto* color=jump?(primitive.kind==PrimitiveKind::PerfectJump?WorkshopJumpGreenColor.data():
            WorkshopJumpAmberColor.data()):primitiveColor(primitive,settings);
        auto* e=create_("prop_dynamic",-1);
        if(!e) throw std::runtime_error("Cannot create flat route marker model. Check the entity limit.");
        const Visual visual{e->GetRefEHandle()};
        auto* kv=new CEntityKeyValues();
        kv->SetString("targetname","routeline_marker_mesh");
        kv->SetString("model",asset.effect);
        kv->SetVector("origin",Vector(position.x,position.y,position.z));
        kv->SetInt("solid",0);
        kv->SetInt("disableshadows",1);
        kv->SetColor("rendercolor",Color(color[0],color[1],color[2],color[3]));
        if(color[3]!=255) {
            kv->SetInt("rendermode",1);
            kv->SetInt("renderamt",color[3]);
        }
        team_.get<uint8_t>(e)=AnnotationTeam;
        spawn_(e,kv);
        e=entity(visual);
        auto* body=e?modelBody_.get<void*>(e):nullptr;
        auto* scene=body?modelScene_.get<void*>(body):nullptr;
        if(!scene||!modelHandle_.get<void*>(static_cast<char*>(scene)+modelState_.offset)) {
            erase(visual);
            throw std::runtime_error("Flat route marker model could not load. Check the RouteLine Workshop addon.");
        }
        return visual;
    }
    Visual createFlatMarkers(const Primitive& primitive,const Settings& settings) {
        const auto& origins=primitive.flatMarkerOrigins;
        constexpr std::array<const char*,FlatMarkerBatchSize> positionKeys{
            "cpoint1_value","cpoint2_value","cpoint3_value","cpoint4_value"};
        const auto* color=primitiveColor(primitive,settings);
        const bool jump=isJumpMarker(primitive.kind);
        const auto& asset=primitive.kind==PrimitiveKind::PerfectJump?WorkshopJumpGreen:
            jump?WorkshopJumpAmber:workshopFlatMarkerAsset(color);
        std::array<Vec3,FlatMarkerBatchSize> positions;
        for(size_t i=0;i<origins.size();++i) positions[i]=flatMarkerPosition(primitive,origins[i],settings);

        auto* e=create_("info_particle_system",-1);
        if(!e) throw std::runtime_error("Cannot create flat route marker. Check the entity limit.");
        const Visual visual{e->GetRefEHandle(),true,true};
        auto* kv=new CEntityKeyValues();
        kv->SetString("targetname","routeline_workshop_flat");
        kv->SetString("effect_name",asset.effect);
        kv->SetVector("origin",Vector(positions[0].x,positions[0].y,positions[0].z));
        kv->SetBool("start_active",false);
        kv->SetBool("no_save",true);
        // CP 1–4 hold world positions and CP 5 supplies the count and alpha.
        kv->SetInt("data_cp",5);
        kv->SetVector("data_cp_value",Vector(float(origins.size()),jump?1.0f:color[3]/255.0f,0));
        if(&asset==&WorkshopFlatMarkerCustom) {
            kv->SetInt("tint_cp",6);
            kv->SetColor("tint_cp_color",Color(color[0],color[1],color[2],color[3]));
        }
        for(size_t i=0;i<origins.size();++i) {
            const auto& p=positions[i];
            std::ostringstream value;
            value.imbue(std::locale::classic());
            value<<std::fixed<<std::setprecision(5)<<p.x<<' '<<p.y<<' '<<p.z;
            kv->SetString(positionKeys[i],value.str().c_str());
        }
        team_.get<uint8_t>(e)=AnnotationTeam;
        spawn_(e,kv);
        e=entity(visual);
        if(e) {
            const variant_t value;
            acceptInput_(e,"Start",nullptr,nullptr,&value);
            e=entity(visual);
        }
        if(!e||!particleEffect_.get<void*>(e)||!particleActive_.get<bool>(e)) {
            erase(visual);
            throw std::runtime_error("Flat route marker could not start. Update the RouteLine Workshop addon and reload the map.");
        }
        return visual;
    }
    Visual createWorkshop(const Primitive& primitive,const Settings& settings) {
        if(!primitive.flatMarkerOrigins.empty())
            return settings.meshMarkers?createMeshMarker(primitive,settings):createFlatMarkers(primitive,settings);
        const bool landing=primitive.kind==PrimitiveKind::Landing;
        std::vector<Vec3> points;
        if(landing) {
            points=workshopMarker(primitive.segment.start,settings,[this](Vec3 p){return probeGround(p);},primitive.ladderContact);
        } else {
            points=primitive.ropeNodes;
            for(auto& point:points) point.z+=settings.zOffset;
        }
        const auto path=workshopPathNodes(points);
        const auto pins=workshopPinnedNodes(points.size());
        auto* e=create_("path_particle_rope",-1);
        if(!e) throw std::runtime_error("Cannot create path_particle_rope. Check the entity limit and server build.");
        const auto handle=e->GetRefEHandle();
        auto* kv=new CEntityKeyValues();
        const auto* color=primitiveColor(primitive,settings);
        const auto& origin=points.front();
        kv->SetString("targetname","routeline_workshop");
        kv->SetString("effect_name",landing?WorkshopMarkerEffect:WorkshopRouteEffect);
        kv->SetVector("origin",Vector(origin.x,origin.y,origin.z));
        kv->SetString("pathNodes",path.c_str());
        kv->SetString("pathNodePinsEnabled",pins.c_str());
        kv->SetInt("closed_loop",0);
        kv->SetBool("start_active",true);
        kv->SetBool("static_collision",false);
        kv->SetFloat("slack",0);
        kv->SetFloat("particle_spacing",landing?settings.markerParticleSpacing:settings.routeParticleSpacing);
        kv->SetFloat("radius",landing?settings.landingWidth:std::min(settings.width*0.6f,2.0f));
        if(primitive.nodeCrouched.empty()) kv->SetColor("color_tint",Color(color[0],color[1],color[2],color[3]));
        else {
            kv->SetColor("color_tint",Color(255,255,255,color[3]));
            kv->SetString("pathNodeColors",workshopNodeColors(primitive.nodeCrouched,settings).c_str());
        }
        team_.get<uint8_t>(e)=AnnotationTeam;
        spawn_(e,kv);
        if(!entity({handle,true})||ropeNodeCount_.get<int32_t>(e)!=static_cast<int32_t>(points.size())
           ||!ropeEffect_.get<void*>(e)) {
            erase({handle,true});
            throw std::runtime_error("Workshop rope rejected its path or effect. Check the addon mount and server build.");
        }
        return {handle,true};
    }
public:
    void init(ISchemaSystem* schema,const std::string& file) {
        std::ifstream f(file);if(!f) throw std::runtime_error("Cannot open routeline/gamedata.json.");const auto data=Json::parse(f).at("linux");
        create_=reinterpret_cast<Create>(resolveSignature(data.at("CreateEntityByName").get<std::string>()));
        spawn_=reinterpret_cast<Spawn>(resolveSignature(data.at("DispatchSpawn").get<std::string>()));
        remove_=reinterpret_cast<Remove>(resolveSignature(data.at("RemoveEntity").get<std::string>()));
        team_.init(schema,"CBaseEntity","m_iTeamNum");
        try {
            ropeNodeCount_.init(schema,"CPathParticleRope","m_PathNodes_Position");
            ropeEffect_.init(schema,"CPathParticleRope","m_iEffectIndex");
            ropeState_.init(schema,"CPathParticleRope","m_nEffectState");
            // Old gamedata files remain usable after a binary-only upgrade.
            const auto inputPattern=data.value("AcceptInput",std::string("55 48 89 E5 41 57 49 89 FF 41 56 48 8D BD ?? ?? ?? ?? 41 55 4C 8D AD"));
            acceptInput_=reinterpret_cast<AcceptInput>(resolveSignature(inputPattern));
            workshopSupported_=true;
        } catch(const std::runtime_error&) {
            workshopSupported_=false;
        }
        try {
            particleActive_.init(schema,"CParticleSystem","m_bActive");
            particleStopType_.init(schema,"CParticleSystem","m_nStopType");
            particleEffect_.init(schema,"CParticleSystem","m_iEffectIndex");
            flatMarkerSupported_=workshopSupported_;
        } catch(const std::runtime_error&) {
            flatMarkerSupported_=false;
        }
        try {
            modelBody_.init(schema,"CBaseEntity","m_CBodyComponent");
            modelScene_.init(schema,"CBodyComponent","m_pSceneNode");
            modelState_.init(schema,"CSkeletonInstance","m_modelState");
            modelHandle_.init(schema,"CModelState","m_hModel");
            meshMarkerSupported_=workshopSupported_;
        } catch(const std::runtime_error&) {
            meshMarkerSupported_=false;
        }
        navVTable_=findServerVTable("CNavPhysicsInterface",4);
        if(navVTable_&&!navVTable_[3]) navVTable_=nullptr;
    }
    bool supportsWorkshop() const {return workshopSupported_;}
    bool supportsFlatMarkers() const {return flatMarkerSupported_;}
    bool supportsMeshMarkers() const {return meshMarkerSupported_;}
    struct Stats {size_t live=0,active=0,total=0,markers=0,jumps=0,perfectJumps=0;};
    Stats stats(int slot) const {
        Stats stats;const auto* group=displays_.group(slot);if(!group) return stats;
        stats.total=displays_.selected(slot).size();
        for(auto i:displays_.selected(slot)) if(const auto& v=group->entries[i].visual;v) if(auto* e=entity(*v)) {
            ++stats.live;
            if(effectState(*v,e)==1) ++stats.active;
            if(group->drawing[i].kind==PrimitiveKind::Landing) {
                stats.markers+=std::max(size_t(1),group->drawing[i].flatMarkerOrigins.size());
            } else if(isJumpMarker(group->drawing[i].kind)) {
                const auto count=group->drawing[i].flatMarkerOrigins.size();
                stats.jumps+=count;
                if(group->drawing[i].kind==PrimitiveKind::PerfectJump) stats.perfectJumps+=count;
            }
        }
        return stats;
    }
    FrameWindow window(int slot) const {return displays_.window(slot);}
    void clear(int slot,bool remove=true,uint64_t steam=0) {
        displays_.clear(slot,[&](const Visual& v){if(remove) erase(v,slot,steam);});
    }
    void clearAll(bool remove=true) {
        for(int slot=0;slot<64;++slot) clear(slot,remove);
        if(!remove) {stopping_.clear();retired_.clear();}
    }
    void finishStops() {
        for(const auto& stop:stopping_) if(auto* e=entity({stop.handle})) removeNow(stop.handle,e);
        stopping_.clear();
    }
    template<class Acknowledged> void collect(Acknowledged acknowledged) {
        const auto now=Clock::now();
        stopping_.erase(std::remove_if(stopping_.begin(),stopping_.end(),[&](const Stopping& stop) {
            auto* e=entity({stop.handle});
            if(!e) return true;
            if(stop.flatMarker) {
                if(now<stop.fallback&&(!stop.firstSnapshotTick||
                    !acknowledged(stop.slot,stop.steam,stop.handle.GetEntryIndex(),*stop.firstSnapshotTick))) return false;
            } else {
                const auto until=stop.firstTransmit?*stop.firstTransmit+std::chrono::milliseconds(350):stop.fallback;
                if(now<until) return false;
            }
            removeNow(stop.handle,e);
            return true;
        }),stopping_.end());
        retired_.erase(std::remove_if(retired_.begin(),retired_.end(),[](CEntityHandle h){return !GameEntitySystem()||!GameEntitySystem()->GetEntityInstance(h);}),retired_.end());
    }
    void attach(int slot,std::shared_ptr<const Route> route,Vec3 position,const Settings& settings,uint64_t steam=0) {
        displays_.attach(slot,std::move(route),position,settings,[&](const Visual& v){erase(v,slot,steam);});
    }
    void confirmLandings(Route& route) {
        route.confirmLandings([this](Vec3 p)->std::optional<float> {
            if(const auto hit=probeGround(p,true)) return hit->point.z;
            return std::nullopt;
        });
    }
    void update(const Settings& settings) {
        if(displays_.groups().empty()||displays_.reserved()+stopping_.size()>settings.totalElements) return;
        const auto share=std::max(size_t(1),settings.createPerFrame/displays_.groups().size());
        size_t remaining=settings.createPerFrame;
        const auto count=displays_.groups().size();
        const auto first=nextGroup_++%count;
        std::array<bool,64> pendingMarkerStops{};
        for(const auto& stop:stopping_) if(stop.flatMarker)
            pendingMarkerStops[stop.slot]=true;
        for(size_t visited=0;visited<count&&remaining;++visited) {
            const auto& group=displays_.groups()[(first+visited)%count];
            size_t budget=std::min(share,remaining);
            size_t checks=std::min(group->active.size(),share*8);
            auto it=group->active.lower_bound(group->cursor);
            while(checks--&&budget&&!group->active.empty()) {
                if(it==group->active.end()) it=group->active.begin();
                const auto n=*it++;group->cursor=n+1;
                auto& entry=group->entries[n];
                if(!entry.visual||!entity(*entry.visual)) {
                    const auto& primitive=group->drawing[n];
                    if(group->privateSlot>=0&&pendingMarkerStops[group->privateSlot]&&!primitive.flatMarkerOrigins.empty())
                        continue;
                    const auto& render=group->settings;
                    entry.visual=createWorkshop(primitive,render);
                    --budget;
                    --remaining;
                }
            }
        }
    }
    template<class F> void each(int snapshotTick,F callback) {
        for(const auto& group:displays_.groups()) for(auto i:group->active) {
            const auto& entry=group->entries[i];
            if(entry.visual&&entity(*entry.visual)) callback(entry.subscribers,entry.visual->handle.GetEntryIndex(),uint64_t(0));
        }
        for(auto& stop:stopping_) if(entity({stop.handle})) {
            const bool transmitted=callback(uint64_t(1)<<stop.slot,stop.handle.GetEntryIndex(),stop.steam);
            if(transmitted&&!stop.firstTransmit) stop.firstTransmit=Clock::now();
            if(transmitted&&snapshotTick>=0&&!stop.firstSnapshotTick) stop.firstSnapshotTick=snapshotTick;
        }
        for(auto h:retired_) if(entity({h})) callback(uint64_t(0),h.GetEntryIndex(),uint64_t(0));
    }
};
} // namespace line
