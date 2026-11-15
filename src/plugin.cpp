#include <ISmmPlugin.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/networkgameserverbase.h>
#include <engine2/igameresourceservice.h>
#include <engine2/igameeventsystem.h>
#include <engine2/gameshareddefs.h>
#include <networksystem/inetworkmessages.h>
#include <icvar.h>
#include <ics2kz.h>
#include <ics2menus.h>
#include <igamesystem.h>
#include "usermessages.pb.h"
#include "cs2kz_provider.h"
#include "renderer.h"
#include "curve.h"
#include <cstddef>
#include <future>
#include <list>
#include <type_traits>

PLUGIN_GLOBALVARS();
namespace {
// CS2 14197 uses a flat message handle without a serializer vtable.
static_assert(!std::is_polymorphic_v<INetworkMessageInternal>);
static_assert(std::is_standard_layout_v<INetworkMessageInternal>);
static_assert(sizeof(INetworkMessageInternal)==40);
static_assert(offsetof(INetworkMessageInternal,m_pfnAllocateMessage)==8);
static_assert(offsetof(INetworkMessageInternal,m_MessageId)==24);
IVEngineServer2* engine=nullptr;
ISource2Server* server=nullptr;
ISource2GameClients* clients=nullptr;
ISource2GameEntities* entities=nullptr;
INetworkServerService* network=nullptr;
IFileSystem* filesystem=nullptr;
ISchemaSystem* schemas=nullptr;
INetworkMessages* messages=nullptr;
IGameEventSystem* events=nullptr;
using namespace line;
bool equal(const char* a,const char* b) {return a&&b&&!V_stricmp(a,b);}
const char* menuTypeName(MenuType type) {
    switch(type) {
        case MenuType::Panorama: return "Panorama";
        case MenuType::Html: return "HTML";
        case MenuType::Chat: return "Chat";
        case MenuType::Default: return "Default";
    }
    return "unknown";
}
std::string clean(std::string text) {
    text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<32||c==127;}),text.end());
    if(text.size()>180) text.resize(180);
    return text;
}
std::string key(const Query& q,const Record& r) {return std::string(q.global?"g:":"s:")+r.id+":"+q.map+":"+q.course+":"+q.mode+(q.pro?":pro:":":overall:")+std::to_string(q.rank);}

class RouteLine final:public ISmmPlugin,public IMetamodListener {
    using Clock=std::chrono::steady_clock;
    enum class RecordMode { Current, Classic, Vanilla };
    struct Session {
        uint64_t steam=0,generation=0;
        bool pending=false;
        bool windowWarning=false;
        Clock::time_point lastRequest{};
        std::shared_ptr<const Route> route;
        std::optional<bool> follow;
        std::optional<DisplayLayers> layers;
        std::optional<bool> global,pro;
        unsigned rank=1;
        RecordMode mode=RecordMode::Current;
        MenuHandle options=kInvalidMenuHandle;
    };
    struct Ticket {int slot;uint64_t steam,generation,epoch;};
    struct Loaded {Ticket ticket;std::shared_ptr<Route> route;std::string error;};
    std::array<Session,64> sessions_;
    uint64_t epoch_=0;
    std::string map_,base_;
    bool active_=false;
    ICS2KZ* kz_=nullptr;
    ICS2Menus* menus_=nullptr;
    CS2KZProvider provider_;
    Renderer renderer_;
    Settings settings_;
    void** manifestVTable_=nullptr;
    bool workshopPrecached_=false;
    bool flatMarkerPrecached_=false;
    bool jumpMarkerPrecached_=false;
    bool meshMarkerPrecached_=false;
    bool follows(int slot) const {return sessions_[slot].follow.value_or(settings_.follow);}
    Settings renderSettings(int slot) const {
        auto result=settings_;
        result.follow=follows(slot);
        result.layers=sessions_[slot].layers.value_or(settings_.layers);
        result.meshMarkers=meshMarkerPrecached_;
        result.takeoffMarkers=result.takeoffMarkers&&(meshMarkerPrecached_||jumpMarkerPrecached_);
        return result;
    }
    std::optional<Vec3> viewPosition(int slot) const {
        KZMovementState movement{};
        if(!kz_||!kz_->GetMovementState(slot,&movement)) return std::nullopt;
        if(!movement.alive) {
            const int target=kz_->GetSpectatedSlot(slot);
            if(target<0||!kz_->GetMovementState(target,&movement)) return std::nullopt;
        }
        return Vec3{movement.origin[0],movement.origin[1],movement.origin[2]};
    }
    std::future<Loaded> worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::list<std::pair<std::string,std::shared_ptr<const Route>>> cache_;
    Clock::time_point nextWindow_{};
    KHook::Virtual<ISource2Server,void,bool,bool,bool> frameHook_{&ISource2Server::GameFrame,this,nullptr,&RouteLine::frame};
    KHook::Virtual<ISource2GameClients,void,CPlayerSlot,const CCommand&> commandHook_{&ISource2GameClients::ClientCommand,this,&RouteLine::command,nullptr};
    KHook::Virtual<ICvar,void,ConCommandRef,const CCommandContext&,const CCommand&> dispatchHook_{&ICvar::DispatchConCommand,this,&RouteLine::dispatch,nullptr};
    KHook::Virtual<ISource2GameEntities,void,CCheckTransmitInfo**,int,CBitVec<16384>&,CBitVec<16384>&,const Entity2Networkable_t**,const uint16*,int> transmitHook_{
        &ISource2GameEntities::CheckTransmit,this,nullptr,&RouteLine::transmit};
    KHook::Virtual<IGameSystem,void,const EventBuildGameSessionManifest_t*> manifestHook_{
        &IGameSystem::OnBuildGameSessionManifest,this,nullptr,&RouteLine::precacheWorkshop};
    KHook::Return<void> precacheWorkshop(IGameSystem*,const EventBuildGameSessionManifest_t* event) {
        if(event&&event->m_pResourceManifest&&renderer_.supportsWorkshop()
           &&filesystem->FileExists(WorkshopRouteFile,"GAME")&&filesystem->FileExists(WorkshopMarkerFile,"GAME")) {
            event->m_pResourceManifest->AddResource(WorkshopRouteEffect);
            event->m_pResourceManifest->AddResource(WorkshopMarkerEffect);
            workshopPrecached_=true;
            if(renderer_.supportsMeshMarkers()&&filesystem->FileExists(WorkshopMarkerMaterial,"GAME")
               &&filesystem->FileExists(WorkshopMarkerMaterialTexture,"GAME")
               &&filesystem->FileExists(WorkshopLandingMesh.file,"GAME")&&filesystem->FileExists(WorkshopJumpMesh.file,"GAME")) {
                event->m_pResourceManifest->AddResource(WorkshopLandingMesh.effect);
                event->m_pResourceManifest->AddResource(WorkshopJumpMesh.effect);
                meshMarkerPrecached_=true;
            }
            if(renderer_.supportsFlatMarkers()&&filesystem->FileExists(WorkshopFlatMarkerTexture,"GAME")
               &&filesystem->FileExists(WorkshopFlatMarkerCustom.file,"GAME")
               &&filesystem->FileExists(WorkshopFlatMarkerBlue.file,"GAME")
               &&filesystem->FileExists(WorkshopFlatMarkerRed.file,"GAME")) {
                for(const auto* asset:{&WorkshopFlatMarkerCustom,&WorkshopFlatMarkerBlue,&WorkshopFlatMarkerRed})
                    event->m_pResourceManifest->AddResource(asset->effect);
                flatMarkerPrecached_=true;
            }
            if(renderer_.supportsFlatMarkers()&&filesystem->FileExists(WorkshopJumpTexture,"GAME")
               &&filesystem->FileExists(WorkshopJumpAmber.file,"GAME")&&filesystem->FileExists(WorkshopJumpGreen.file,"GAME")) {
                event->m_pResourceManifest->AddResource(WorkshopJumpAmber.effect);
                event->m_pResourceManifest->AddResource(WorkshopJumpGreen.effect);
                jumpMarkerPrecached_=true;
            }
        }
        return {KHook::Action::Ignore};
    }
    const char* workshopError(const Settings& settings) const {
        if(settings.backend!=RenderBackend::Workshop) return nullptr;
        if(!manifestVTable_||!renderer_.supportsWorkshop()) return "Workshop rendering is unavailable on this server build.";
        if(!workshopPrecached_) return "Mount the RouteLine Workshop addon and reload the map before using this profile.";
        if(settings.useFlatMarkers()&&!meshMarkerPrecached_) {
            if(!renderer_.supportsFlatMarkers()) return "Flat landing markers are unavailable on this server build.";
            if(!flatMarkerPrecached_) return "Update the RouteLine Workshop addon and reload the map to use flat landing markers.";
        }
        return nullptr;
    }
    uint64_t steam(int slot) const {
        if(slot<0||slot>=64||!kz_||!kz_->IsValidPlayer(slot)||!engine->IsClientFullyAuthenticated(CPlayerSlot(slot))) return 0;
        const auto* id=engine->GetClientSteamID(CPlayerSlot(slot));return id&&id->BIndividualAccount()?id->ConvertToUint64():0;
    }
    void say(int slot,const std::string& text) {
        const auto out="[RouteLine] "+clean(text);
        if(slot<0||slot>=64) {META_CONPRINTF("%s\n",out.c_str());return;}
        engine->ClientPrintf(CPlayerSlot(slot),(out+"\n").c_str());
        auto* type=messages->FindNetworkMessageById(UM_TextMsg);if(!type||!type->m_pfnAllocateMessage) return;
        auto* allocated=type->AllocateMessage();if(!allocated) return;
        auto* msg=allocated->ToPB<CUserMessageTextMsg>();msg->set_dest(3);msg->add_param(out);
        for(int i=0;i<4;++i) msg->add_param("");
        uint64 mask=uint64(1)<<slot;events->PostEventAbstract(0,false,1,&mask,type,msg,0,BUF_RELIABLE);delete msg;
    }
    void clear(int slot,bool remove=true,bool keepDisplayPreference=false) {
        auto& s=sessions_[slot];
        const auto owner=s.steam;
        ++s.generation;
        s.pending=false;
        s.windowWarning=false;
        s.route.reset();
        if(!keepDisplayPreference) {
            closeOptions(slot);
            s.steam=0;
            s.follow.reset();
            s.layers.reset();
            s.global.reset();
            s.pro.reset();
            s.rank=1;
            s.mode=RecordMode::Current;
        }
        renderer_.clear(slot,remove,owner);
    }
    void reset(bool remove) {
        ++epoch_;if(cancel_) cancel_->store(true);
        for(int i=0;i<64;++i) clear(i,remove);
        if(!remove) renderer_.clearAll(false);
        cache_.clear();
    }
    bool valid(const Ticket& t) const {
        return t.slot>=0&&t.slot<64&&t.epoch==epoch_&&t.steam&&t.steam==steam(t.slot)
            &&sessions_[t.slot].steam==t.steam&&sessions_[t.slot].generation==t.generation;
    }
    std::shared_ptr<const Route> cached(const std::string& id) {
        for(auto it=cache_.begin();it!=cache_.end();++it) if(it->first==id) {
            auto result=it->second;cache_.splice(cache_.begin(),cache_,it);return result;
        }
        // Active displays can outlive their LRU entry. Reuse them rather than decode
        // the same route again and allocate a second set of persistent entities.
        for(const auto& session:sessions_) if(session.route&&key(session.route->query,session.route->record)==id) return session.route;
        return {};
    }
    void show(const Ticket& t,std::shared_ptr<const Route> route) {
        if(!valid(t)) return;
        auto& s=sessions_[t.slot];s.pending=false;
        auto render=renderSettings(t.slot);
        const auto position=render.follow?viewPosition(t.slot):std::nullopt;
        if(!position) {render.routeDistance=0;render.markerDistance=0;}
        try {renderer_.attach(t.slot,route,position.value_or(Vec3{}),render,s.steam);}
        catch(const std::exception& e) {say(t.slot,e.what());return;}
        s.route=std::move(route);s.windowWarning=false;
        const auto& q=s.route->query;const auto& r=s.route->record;
        say(t.slot,std::string(q.global?"GLOBAL #":"SR #")+std::to_string(q.rank)+" / "+(q.pro?"PRO":"OVERALL")+" / "+q.mode+" / "+q.course
            +" | "+r.player+" | "+std::to_string(r.time)+"s | !line off");
    }
    void received(Ticket ticket,Query q,Record record,IRouteProvider::Input input,std::string error) {
        if(!valid(ticket)) return;
        if(!error.empty()) {sessions_[ticket.slot].pending=false;say(ticket.slot,error);return;}
        if(auto route=cached(key(q,record))) {show(ticket,route);return;}
        if(worker_.valid()) {sessions_[ticket.slot].pending=false;say(ticket.slot,"Another route is being decoded. Try again shortly.");return;}
        const auto options=settings_.decode;cancel_=std::make_shared<std::atomic<bool>>(false);auto cancel=cancel_;
        worker_=std::async(std::launch::async,[ticket,q,record,input=std::move(input),options,cancel]() mutable {
            Loaded result{ticket,{}, {}};
            try {
                if(!input.path.empty()) {
                    std::ifstream file(input.path,std::ios::binary|std::ios::ate);
                    const auto size=file.tellg();
                    if(!file||size<=0||uint64_t(size)>options.maxFileBytes) throw std::runtime_error("Cannot read replay, or file exceeds the size limit.");
                    input.bytes.resize(size_t(size));file.seekg(0);
                    if(!file.read(input.bytes.data(),std::streamsize(size))) throw std::runtime_error("Replay file was removed or changed while reading.");
                }
                if(!input.decode) throw std::runtime_error("Timer provider did not supply a replay decoder.");
                result.route=std::make_shared<Route>(input.decode(input.bytes,q,record,options,cancel.get()));
            }catch(const std::exception& e) {result.error=e.what();}
            return result;
        });
    }
    void refresh() {
        refreshMenus();
        auto* kz=static_cast<ICS2KZ*>(g_SMAPI->MetaFactory(CS2KZ_INTERFACE,nullptr,nullptr));
        if(kz!=kz_) {reset(active_);kz_=kz;}
        provider_.refreshSQL(static_cast<ISQLInterface*>(g_SMAPI->MetaFactory(SQLMM_INTERFACE,nullptr,nullptr)));
    }
    void refreshMenus() {
        auto* menus=static_cast<ICS2Menus*>(g_SMAPI->MetaFactory(CS2MENUS_INTERFACE,nullptr,nullptr));
        if(menus==menus_) return;
        // The old provider may already have unloaded and owns all its handles.
        for(auto& session:sessions_) session.options=kInvalidMenuHandle;
        menus_=menus;
    }
    void closeOptions(int slot) {
        const auto menu=sessions_[slot].options;
        sessions_[slot].options=kInvalidMenuHandle;
        if(menus_&&menu) menus_->DestroyMenu(menu);
    }
    bool validOptions(MenuHandle menu,int slot,uint64_t identity) const {
        return active_&&menus_&&slot>=0&&slot<64&&sessions_[slot].options==menu
            &&sessions_[slot].steam==identity&&steam(slot)==identity;
    }
    template<class Change> bool changeDisplay(int slot,Change change) {
        const auto identity=steam(slot);
        if(!active_||!identity) {say(slot,"Requires an active map and an authenticated player.");return false;}
        auto& session=sessions_[slot];
        if(session.steam!=identity) {clear(slot);session.steam=identity;}
        auto render=renderSettings(slot);
        const bool follow=render.follow;
        const auto layers=render.layers;
        change(render);
        if(session.route&&(render.follow!=follow||render.layers!=layers)) {
            const auto position=render.follow?viewPosition(slot):std::optional<Vec3>{Vec3{}};
            if(!position) {say(slot,"Player position unavailable.");return false;}
            try {renderer_.attach(slot,session.route,*position,render,identity);}
            catch(const std::exception& e) {say(slot,e.what());return false;}
        }
        session.follow=render.follow;
        session.layers=render.layers;
        session.windowWarning=false;
        return true;
    }
    static bool DisplayLayers::* menuLayer(const char* name) {
        if(equal(name,"line")) return &DisplayLayers::path;
        if(equal(name,"jump")) return &DisplayLayers::jumps;
        if(equal(name,"landing")) return &DisplayLayers::landings;
        return nullptr;
    }
    void syncOptions(int slot) {
        refreshMenus();
        const auto menu=sessions_[slot].options;
        if(!menu) return;
        const auto render=renderSettings(slot);
        for(int item=0;item<menus_->GetItemCount(menu);++item) {
            const auto* name=menus_->GetItemInfo(menu,item);
            if(equal(name,"follow")) menus_->SetItemValue(menu,item,render.follow?0:1);
            else if(const auto layer=menuLayer(name)) menus_->SetItemValue(menu,item,render.layers.*layer);
        }
    }
    Query defaultQuery(int slot,bool useOptions) const {
        Query q;
        q.map=map_;
        q.course.clear();
        q.global=settings_.global;
        q.pro=settings_.pro;
        KZTimerStatus status{};
        if(kz_->GetTimerStatus(slot,&status)) {
            q.mode=equal(status.modeShortName,"VNL")?"vanilla":"classic";
            if(status.onCourse) q.course=status.course.name;
        }
        if(useOptions) {
            const auto& session=sessions_[slot];
            q.global=session.global.value_or(q.global);
            q.pro=session.pro.value_or(q.pro);
            q.rank=session.rank;
            if(session.mode==RecordMode::Classic) q.mode="classic";
            if(session.mode==RecordMode::Vanilla) q.mode="vanilla";
        }
        return q;
    }
    void requestRoute(int slot,Query query) {
        const auto identity=steam(slot);
        if(!active_||!identity) {say(slot,"Requires an active map and an authenticated player.");return;}
        if(const char* error=workshopError(settings_)) {say(slot,error);return;}
        auto& session=sessions_[slot];
        const auto now=Clock::now();
        if(now-session.lastRequest<std::chrono::seconds(2)) {say(slot,"Wait two seconds between route requests.");return;}
        session.lastRequest=now;
        if(session.steam!=identity) clear(slot);else ++session.generation;
        session.steam=identity;
        session.pending=true;
        Ticket ticket{slot,identity,session.generation,epoch_};
        say(slot,"Loading #"+std::to_string(query.rank)+" route...");
        provider_.request(query,[this,ticket](Query resolved,Record record,IRouteProvider::Input input,std::string error) {
            received(ticket,std::move(resolved),std::move(record),std::move(input),std::move(error));
        });
    }
    void openOptions(int slot,MenuType type) {
        refreshMenus();
        const auto identity=steam(slot);
        if(!active_||!identity) {say(slot,"Requires an active map and an authenticated player.");return;}
        if(!menus_) {say(slot,"HUD options require CS2Menus (ICS2Menus008). Use !line help for chat commands.");return;}
        if(menus_->GetExternalBusy(slot)) {
            say(slot,"Another HUD menu is open. Close it, then use !line options again.");
            return;
        }
        auto& session=sessions_[slot];
        if(session.steam!=identity) {clear(slot);session.steam=identity;}
        closeOptions(slot);
        const auto menu=menus_->CreateMenu(type,"RouteLine Options",[this,identity](MenuHandle handle,int player,int item) {
            if(!validOptions(handle,player,identity)) return;
            const char* action=menus_->GetItemInfo(handle,item);
            if(equal(action,"show")) requestRoute(player,defaultQuery(player,true));
            else if(equal(action,"hide")) {clear(player,true,true);say(player,"Route off.");}
        });
        if(!menu) {say(slot,"Could not create the RouteLine options menu.");return;}
        session.options=menu;
        if(type!=MenuType::Default) menus_->SetMenuForceType(menu,true);
        menus_->SetMenuEndCallback(menu,[this](MenuHandle handle,int player,MenuEndReason) {
            if(player>=0&&player<64&&sessions_[player].options==handle) closeOptions(player);
        });
        menus_->SetMenuChangeCallback(menu,[this,identity](MenuHandle handle,int player,int item,int value) {
            if(!validOptions(handle,player,identity)) return;
            auto& s=sessions_[player];
            const char* name=menus_->GetItemInfo(handle,item);
            if(equal(name,"source")) s.global=value==0;
            else if(equal(name,"category")) s.pro=value==0;
            else if(equal(name,"mode")) s.mode=static_cast<RecordMode>(value);
            else if(equal(name,"rank")) s.rank=unsigned(value);
            else if(equal(name,"follow")) {
                if(!changeDisplay(player,[value](Settings& render){render.follow=value==0;}))
                    menus_->SetItemValue(handle,item,follows(player)?0:1);
            } else if(const auto layer=menuLayer(name)) {
                if(!changeDisplay(player,[layer,value](Settings& render){render.layers.*layer=value!=0;}))
                    menus_->SetItemValue(handle,item,renderSettings(player).layers.*layer);
            }
        });
        const auto render=renderSettings(slot);
        const auto query=defaultQuery(slot,true);
        constexpr const char* sources[]={"Global","Server"};
        constexpr const char* categories[]={"PRO","Overall"};
        constexpr const char* modes[]={"Current","Classic","Vanilla"};
        constexpr const char* ranges[]={"Follow","All"};
        menus_->AddItem(menu,"Show / replace route","show",false);
        menus_->AddItem(menu,"Hide route","hide",false);
        menus_->AddChoice(menu,"Source",sources,2,query.global?0:1,"source");
        menus_->AddChoice(menu,"Category",categories,2,query.pro?0:1,"category");
        menus_->AddChoice(menu,"Mode",modes,3,int(session.mode),"mode");
        menus_->AddStepper(menu,"Rank",int(query.rank),1,int(MaxRecordRank),1,"rank");
        menus_->AddChoice(menu,"Range",ranges,2,render.follow?0:1,"follow");
        if(render.detail!=DisplayDetail::Landings) menus_->AddToggle(menu,"Line",render.layers.path,"line");
        if(render.useTakeoffMarkers()) menus_->AddToggle(menu,"Jump marks",render.layers.jumps,"jump");
        if(render.landingMarkers) menus_->AddToggle(menu,"Landing marks",render.layers.landings,"landing");
        if(!menus_->DisplayMenu(menu,slot,45.0f)) {
            closeOptions(slot);
            say(slot,"Could not display options. Close any other menu and try again.");
            return;
        }
        // DisplayMenu can report success even when the client cannot draw that type.
        const std::string result=menus_->GetActiveMenu(slot)==menu
            ?std::string("Options opened (")+menuTypeName(menus_->GetActiveMenuType(slot))+")."
            :"CS2Menus accepted the options request.";
        engine->ClientPrintf(CPlayerSlot(slot),("[RouteLine] "+result+"\n").c_str());
    }
    bool chat(int slot,const CCommand& args) {
        if(!equal(args.Arg(0),"say")&&!equal(args.Arg(0),"say_team")) return false;
        CCommand words;words.Tokenize(args.ArgC()>2?args.ArgS():args.Arg(1));const char* name=words.Arg(0);
        if((name[0]!='!'&&name[0]!='/')||!equal(name+1,"line")) return false;
        run(slot,words);return true;
    }
    KHook::Return<void> dispatch(ICvar*,ConCommandRef,const CCommandContext& context,const CCommand& args) {
        return {chat(context.GetPlayerSlot().Get(),args)?KHook::Action::Supersede:KHook::Action::Ignore};
    }
    KHook::Return<void> command(ISource2GameClients*,CPlayerSlot slot,const CCommand& args) {
        if(equal(args.Arg(0),"routeline")) {run(slot.Get(),args);return {KHook::Action::Supersede};}
        return {KHook::Action::Ignore};
    }
    KHook::Return<void> frame(ISource2Server*,bool,bool,bool) {
        refresh();
        struct Ack {uint64_t owner=0;int tick=-1;const CBitVec<MAX_EDICTS>* entities=nullptr;bool read=false;};
        std::array<Ack,64> acknowledgements{};
        renderer_.collect([&](int slot,uint64_t formerOwner,int index,int firstStoppedTick) {
            auto& ack=acknowledgements[slot];
            if(!ack.read) {
                const CPlayerSlot player(slot);
                ack.owner=steam(slot);
                if(ack.owner) {
                    ack.tick=engine->GetClientDeltaTick(player);
                    ack.entities=engine->GetEntityTransmitBitsForClient(player);
                }
                ack.read=true;
            }
            if(ack.owner!=formerOwner) return true; // The old client has disconnected.
            return ack.tick>=firstStoppedTick&&ack.entities&&ack.entities->IsBitSet(index);
        });
        for(int i=0;i<64;++i) if(sessions_[i].steam&&steam(i)!=sessions_[i].steam) clear(i,active_);
        provider_.frame();
        if(worker_.valid()&&worker_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            auto loaded=worker_.get();
            if(valid(loaded.ticket)) {
                if(!loaded.error.empty()) {sessions_[loaded.ticket.slot].pending=false;say(loaded.ticket.slot,loaded.error);}
                else {
                    renderer_.confirmLandings(*loaded.route);
                    const auto id=key(loaded.route->query,loaded.route->record);
                    cache_.emplace_front(id,loaded.route);while(cache_.size()>settings_.cacheRoutes) cache_.pop_back();
                    show(loaded.ticket,loaded.route);
                }
            }
        }
        if(!active_||!kz_) return {KHook::Action::Ignore};
        if(Clock::now()>=nextWindow_) {
            nextWindow_=Clock::now()+std::chrono::milliseconds(int(settings_.windowInterval*1000));
            for(int slot=0;slot<64;++slot) if(sessions_[slot].route&&follows(slot)) {
                const auto position=viewPosition(slot);
                if(!position) continue;
                auto& session=sessions_[slot];
                try {renderer_.attach(slot,session.route,*position,renderSettings(slot),session.steam);session.windowWarning=false;}
                catch(const std::exception& e) {if(!session.windowWarning) say(slot,e.what());session.windowWarning=true;}
            }
        }
        try {
            renderer_.update(settings_);
        }catch(const std::exception& e) {
            META_CONPRINTF("[RouteLine] Renderer stopped: %s\n",e.what());
            for(int slot=0;slot<64;++slot) if(sessions_[slot].route) say(slot,std::string("Renderer stopped: ")+e.what());
            reset(true);
        }
        return {KHook::Action::Ignore};
    }
    KHook::Return<void> transmit(ISource2GameEntities*,CCheckTransmitInfo** info,int count,CBitVec<16384>& unionTransmit,CBitVec<16384>&,
                                const Entity2Networkable_t**,const uint16*,int) {
        uint64_t authenticated=0;
        std::array<uint64_t,64> online{};
        for(int i=0;i<count;++i) if(info[i]) {
            const int slot=info[i]->m_nPlayerSlot.Get();
            if(slot<0||slot>=64) continue;
            online[slot]=steam(slot);
            if(online[slot]&&online[slot]==sessions_[slot].steam) authenticated|=uint64_t(1)<<slot;
        }
        const auto* globals=engine->GetServerGlobals();
        renderer_.each(globals?globals->tickcount:-1,[&](uint64_t subscribers,int index,uint64_t formerOwner) {
            bool transmitted=false;
            for(int i=0;i<count;++i) if(info[i]&&info[i]->m_pTransmitEntity) {
                const int slot=info[i]->m_nPlayerSlot.Get();
                uint64_t allowed=authenticated;
                if(formerOwner) allowed=slot>=0&&slot<64&&online[slot]==formerOwner?uint64_t(1)<<slot:0;
                if(filterAnnotationTransmission(info[i]->m_pTransmitEntity,info[i]->m_pTransmitAlways,info[i]->m_pTransmitOutOfPVS,
                    index,subscribers,slot,allowed,formerOwner!=0)) transmitted=true;
            }
            if(formerOwner&&transmitted) unionTransmit.Set(index);
            return transmitted;
        });
        return {KHook::Action::Ignore};
    }
public:
    void run(int slot,const CCommand& args) {
        if(slot<0||slot>=64) {say(slot,"Use routeline in a player's console; routeline_reload [profile] in the server console.");return;}
        if(args.ArgC()>1&&(equal(args.Arg(1),"o")||equal(args.Arg(1),"options"))) {
            auto type=MenuType::Default;
            if(args.ArgC()==3&&equal(args.Arg(2),"panorama")) type=MenuType::Panorama;
            else if(args.ArgC()==3&&equal(args.Arg(2),"html")) type=MenuType::Html;
            else if(args.ArgC()==3&&equal(args.Arg(2),"chat")) type=MenuType::Chat;
            else if(args.ArgC()!=2) {say(slot,"Usage: !line options [panorama|html|chat]");return;}
            openOptions(slot,type);
            return;
        }
        if(args.ArgC()>1&&equal(args.Arg(1),"reload")) {
            if(args.ArgC()>3) {say(slot,"Usage: !line reload [profile]");return;}
            reload(slot,args.ArgC()==3?std::string(args.Arg(2)):settings_.profile);
            return;
        }
        if(args.ArgC()>1&&equal(args.Arg(1),"render")) {
            const auto* option=args.Arg(2);
            if(args.ArgC()==3) {
                if(equal(option,"follow")||equal(option,"all")) {
                    const bool follow=equal(option,"follow");
                    if(changeDisplay(slot,[follow](Settings& render){render.follow=follow;})) {
                        syncOptions(slot);
                        say(slot,follow?"Display: follow.":"Display: all.");
                    }
                    return;
                }
                // Old bindings still apply the corresponding layer combination.
                if(equal(option,"landings")||equal(option,"full")) {
                    const bool full=equal(option,"full");
                    if(changeDisplay(slot,[full](Settings& render){render.layers={full,full,true};})) {
                        syncOptions(slot);
                        say(slot,full?"Line, jump marks and landing marks on.":"Only landing marks on.");
                    }
                    return;
                }
                if(equal(option,"workshop")) {
                    say(slot,"Renderer: workshop. Use !line options to change display.");return;
                }
            }
            const auto layer=menuLayer(option);
            const bool on=equal(args.Arg(3),"on");
            if(args.ArgC()!=4||!layer||(!on&&!equal(args.Arg(3),"off"))) {
                say(slot,"Usage: !line render <all|follow> or !line render <line|jump|landing> <on|off>");
                return;
            }
            if(changeDisplay(slot,[layer,on](Settings& render){render.layers.*layer=on;})) {
                syncOptions(slot);
                say(slot,std::string(option)+(on?" on.":" off."));
            }
            return;
        }
        if(args.ArgC()>1&&equal(args.Arg(1),"help")) {
            say(slot,"!line [g|global|sr [rank]] [pro|overall] [ckz|vnl] [\"course\"] | !line preview | !line off | !line status");
            say(slot,"!line options [panorama|html|chat] | !line render <all|follow> | !line render <line|jump|landing> <on|off> | !line reload [profile] (admin)");return;
        }
        if(args.ArgC()>1&&equal(args.Arg(1),"status")) {
            const auto render=renderSettings(slot);
            const auto& s=sessions_[slot];
            const auto stats=renderer_.stats(slot);
            const auto window=renderer_.window(slot);
            const bool workshop=true;
            const char* units=render.useFlatMarkers()?". Effects: ":workshop?". Paths: ":". Beams: ";
            say(slot,s.pending?"Loading...":s.route?"Landings: "+std::to_string(s.route->landings.size())+" ("+std::to_string(s.route->recoveredLandings)+" inferred). Frames: "+std::to_string(window.begin)+"-"+std::to_string(window.end)
                +units+std::to_string(stats.live)+"/"+std::to_string(stats.total)+", on: "+std::to_string(stats.active)+", width: "+std::to_string(settings_.width)+".":"Route is off.");
            auto rgb=[&](size_t n) {return std::to_string(settings_.colors[n])+"/"+std::to_string(settings_.colors[n+1])+"/"+std::to_string(settings_.colors[n+2]);};
            const char* detail=settings_.detail==DisplayDetail::Full?"full":settings_.detail==DisplayDetail::Compact?"compact":"landings";
            say(slot,"RGB route: "+rgb(0)+", landing: "+rgb(4)+", crouch: "+rgb(8)+". Renderer: "+backendName(render.backend)+". Detail: "+detail+". Path: "+(settings_.decode.smoothPath?"smooth":"precise")+", flatness: "+std::to_string(settings_.decode.curveFlatness)+".");
            if(render.follow) {
                const auto handoff=render.windowOverlap?". Advance: "+std::to_string(render.replayFrames-render.windowOverlap)+" frames":". Centered";
                say(slot,"Display: follow. Limit: "+std::to_string(render.replayFrames)+" frames"+handoff+".");
            } else say(slot,"Display: all (fixed).");
            say(slot,std::string("Layers: line ")+(render.layers.path?"on":"off")+", jump "+(render.layers.jumps?"on":"off")
                +", landing "+(render.layers.landings?"on":"off")+".");
            if(render.follow)
                say(slot,"Follow distance: line "+std::to_string(render.routeDistance)+", flat marks "
                    +std::to_string(render.flatMarkerDistance())+" units (0 = unlimited).");
            if(s.route&&render.follow) say(slot,settings_.separateSections?"Teleport sections: separate.":"Teleport sections: combined.");
            if(s.route&&s.route->replayVersion) say(slot,settings_.decode.hideFailedAttempts?
                "Retry frames hidden: "+std::to_string(s.route->discardedAttemptFrames)+".":"Retry filtering: off.");
            if(const char* error=workshopError(render)) say(slot,error);
            else if(workshop) say(slot,"Contact markers: "+std::to_string(stats.markers)+(render.useFlatMarkers()?
                ". Flat 20x20, blue standing / red crouched.":settings_.detail==DisplayDetail::Full?
                ". Fixed 20x20, blue standing / red crouched.":". Short marks at recorded contacts."));
            if(render.useFlatMarkers()||render.useTakeoffMarkers()) say(slot,render.meshMarkers?
                "Marker renderer: mesh.":"Marker renderer: legacy particles. Update the RouteLine Workshop addon for mesh markers.");
            if(render.layers.jumps&&settings_.useTakeoffMarkers()) {
                if(render.useTakeoffMarkers()) say(slot,"Jump marks: "+std::to_string(stats.jumps)+" ("+std::to_string(stats.perfectJumps)
                    +" inferred perfect). X: green for Classic perfect, amber otherwise.");
                else say(slot,"Update the RouteLine Workshop addon and reload the map to show jump marks.");
            }
            if(render.useFlatMarkers()) say(slot,"Route particle spacing: "+std::to_string(settings_.routeParticleSpacing)+" units.");
            else if(workshop) say(slot,"Particle spacing route/marker: "+std::to_string(settings_.routeParticleSpacing)+" / "+std::to_string(settings_.markerParticleSpacing)+" units.");
            say(slot,"Profile: "+settings_.profile+". Route batching: "+(render.packedRoute?"packed":"standard")
                +". Batch frames: "+std::to_string(std::min(render.pathBatchFrames,render.replayFrames/4))+".");
            return;
        }
        if((args.ArgC()>1&&equal(args.Arg(1),"off"))||(args.ArgC()==1&&(sessions_[slot].route||sessions_[slot].pending))) {
            clear(slot,true,true);say(slot,"Route off.");return;
        }
        const auto identity=steam(slot);
        if(!active_||!identity) {say(slot,"Requires an active map, CS2KZ (ICS2KZ002), and an authenticated player.");return;}
        if(args.ArgC()==2&&equal(args.Arg(1),"preview")) {
            if(const char* error=workshopError(settings_)) {say(slot,error);return;}
            KZMovementState movement{};
            if(!kz_->GetMovementState(slot,&movement)||!movement.alive||!movement.onGround) {say(slot,"Stand on the ground to preview the route.");return;}
            const float yaw=movement.eyeAngles[1]*0.0174532925199433f;
            const Vec3 forward{std::cos(yaw),std::sin(yaw),0};
            const Vec3 start=Vec3{movement.origin[0],movement.origin[1],movement.origin[2]}+forward*24;
            auto route=std::make_shared<Route>();route->query.map=map_;route->query.course="Route preview";
            std::vector<PathSample> guide;
            const Vec3 side{-forward.y,forward.x,0};
            for(uint32_t i=0;i<=128;++i) {
                const float t=i/128.0f;
                guide.push_back({start+forward*(256*t)+side*(32*std::sin(t*6.28318530718f)),i,0,i>=43&&i<85,false,i==0||i==64||i==128,true});
            }
            route->landings={{guide[0].position,0,false,0,0},{guide[64].position,64,true,0,0},{guide[128].position,128,false,0,0}};
            route->jumps={{guide[24].position,24,0,false},{guide[104].position,104,0,true}};
            auto& session=sessions_[slot];
            if(session.steam!=identity) clear(slot);else ++session.generation;
            session.steam=identity;session.pending=false;
            try {
                route->path=smoothPath(guide,settings_.decode.pathXYError,settings_.decode.pathZError,settings_.decode.curveFlatness,
                    settings_.decode.maxPathLength,settings_.decode.maxSegments,nullptr,&route->pathCurves);
                renderer_.attach(slot,route,start,renderSettings(slot),session.steam);session.route=std::move(route);say(slot,"RouteLine preview. !line off to clear.");
            }
            catch(const std::exception& e) {say(slot,e.what());}
            return;
        }
        auto q=defaultQuery(slot,args.ArgC()==1&&sessions_[slot].steam==identity);
        bool courseGiven=false,rankGiven=false;
        for(int i=1;i<args.ArgC();++i) {
            const char* arg=args.Arg(i);
            const bool global=equal(arg,"g")||equal(arg,"global")||equal(arg,"wr");
            if(global||equal(arg,"sr")||equal(arg,"local")) {
                q.global=global;
                if(i+1<args.ArgC()) try {
                    if(const auto rank=recordRankArgument(args.Arg(i+1))) {
                        if(rankGiven) {say(slot,"Specify one rank.");return;}
                        q.rank=*rank;
                        rankGiven=true;
                        ++i;
                    }
                } catch(const std::invalid_argument& e) {say(slot,e.what());return;}
            }
            else if(equal(arg,"pro")) q.pro=true;
            else if(equal(arg,"overall")||equal(arg,"tp")) q.pro=false;
            else if(equal(arg,"ckz")||equal(arg,"classic")) q.mode="classic";
            else if(equal(arg,"vnl")||equal(arg,"vanilla")) q.mode="vanilla";
            else if(!courseGiven&&std::strlen(arg)<=255) {q.course=arg;courseGiven=true;}
            else {say(slot,"Invalid arguments. Use !line help; quote course names containing spaces.");return;}
        }
        requestRoute(slot,std::move(q));
    }
    void reload(int slot,const std::string& profile) {
        if(slot!=-1&&!settings_.canReloadFromChat(steam(slot))) {
            say(slot,"RouteLine reload requires a SteamID64 listed in reload_admins.");return;
        }
        if(provider_.busySQL()) {say(slot,"A database operation is still active. Retry after it finishes.");return;}
        try {
            auto settings=readSettings(base_+"/addons/routeline/config.json",profile);
            if(active_) if(const char* error=workshopError(settings)) throw std::runtime_error(error);
            // Read connections into a temporary provider before replacing a working configuration.
            CS2KZProvider candidate;candidate.configure(base_,filesystem,settings.decode.maxFileBytes);
            reset(active_);provider_.shutdown();provider_=std::move(candidate);settings_=settings;
            provider_.isCached=[this](const Query& q,const Record& r){return bool(cached(key(q,r)));};
            refresh();say(slot,"Reloaded profile "+settings_.profile+". Use !line to show a route again.");
        }catch(const std::exception& e) {say(slot,e.what());}
    }
    bool Load(PluginId id,ISmmAPI* ismm,char* error,size_t maxlen,bool late) override {
        PLUGIN_SAVEVARS();
        if(!KHook::__exported__khook) {std::snprintf(error,maxlen,"Requires Metamod API 18 with KHook.");return false;}
        GET_V_IFACE_CURRENT(GetEngineFactory,g_pCVar,ICvar,CVAR_INTERFACE_VERSION);
        GET_V_IFACE_CURRENT(GetEngineFactory,engine,IVEngineServer2,INTERFACEVERSION_VENGINESERVER);
        GET_V_IFACE_ANY(GetServerFactory,server,ISource2Server,INTERFACEVERSION_SERVERGAMEDLL);
        GET_V_IFACE_ANY(GetServerFactory,clients,ISource2GameClients,INTERFACEVERSION_SERVERGAMECLIENTS);
        GET_V_IFACE_ANY(GetServerFactory,entities,ISource2GameEntities,INTERFACEVERSION_SERVERGAMEENTS);
        GET_V_IFACE_CURRENT(GetEngineFactory,network,INetworkServerService,NETWORKSERVERSERVICE_INTERFACE_VERSION);
        GET_V_IFACE_CURRENT(GetFileSystemFactory,filesystem,IFileSystem,FILESYSTEM_INTERFACE_VERSION);
        GET_V_IFACE_CURRENT(GetEngineFactory,schemas,ISchemaSystem,SCHEMASYSTEM_INTERFACE_VERSION);
        GET_V_IFACE_CURRENT(GetEngineFactory,messages,INetworkMessages,NETWORKMESSAGES_INTERFACE_VERSION);
        GET_V_IFACE_CURRENT(GetEngineFactory,events,IGameEventSystem,GAMEEVENTSYSTEM_INTERFACE_VERSION);
        GET_V_IFACE_CURRENT(GetEngineFactory,g_pGameResourceServiceServer,IGameResourceService,GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
        try {
            base_=g_SMAPI->GetBaseDir();settings_=readSettings(base_+"/addons/routeline/config.json",{});
            provider_.configure(base_,filesystem,settings_.decode.maxFileBytes);
            renderer_.init(schemas,base_+"/addons/routeline/gamedata.json");
            manifestVTable_=findServerVTable("CEntityDebugGameSystem",8);
        }catch(const std::exception& e) {std::snprintf(error,maxlen,"%s",e.what());return false;}
        provider_.isCached=[this](const Query& q,const Record& r){return bool(cached(key(q,r)));};
        if(auto* game=network->GetIGameServer()) {
            const char* name=game->GetMapName();map_=name?name:"";
            const auto slash=map_.find_last_of('/');if(slash!=std::string::npos) map_=map_.substr(slash+1);
        }
        active_=late&&!map_.empty();g_SMAPI->AddListener(this,this);
        frameHook_.Add(server);commandHook_.Add(clients);dispatchHook_.Add(g_pCVar);transmitHook_.Add(entities);
        if(manifestVTable_) manifestHook_.AddGlobal(reinterpret_cast<IGameSystem*>(&manifestVTable_));
        ConVar_Register(FCVAR_RELEASE|FCVAR_GAMEDLL);refresh();
        META_CONPRINTF("[RouteLine] Loaded 0.16.19. Use !line help. Linux / CS2KZ / replay versions 1-7.\n");return true;
    }
    void AllPluginsLoaded() override {refresh();}
    void OnPluginLoad(PluginId) override {refresh();}
    void OnPluginUnload(PluginId) override {refresh();}
    void OnLevelInit(const char* map,const char*,const char*,const char*,bool,bool) override {
        reset(false);map_=map?map:"";const auto slash=map_.find_last_of('/');if(slash!=std::string::npos) map_=map_.substr(slash+1);active_=true;
    }
    void OnLevelShutdown() override {active_=false;workshopPrecached_=flatMarkerPrecached_=jumpMarkerPrecached_=meshMarkerPrecached_=false;reset(false);map_.clear();}
    bool Unload(char* error,size_t maxlen) override {
        if(provider_.busySQL()) {std::snprintf(error,maxlen,"A SQLMM callback is pending. Retry unloading after the database operation finishes.");return false;}
        refreshMenus();reset(active_);renderer_.finishStops();if(worker_.valid()) worker_.get();provider_.shutdown();
        if(manifestVTable_) manifestHook_.RemoveGlobal(reinterpret_cast<IGameSystem*>(&manifestVTable_));
        transmitHook_.Remove(entities);dispatchHook_.Remove(g_pCVar);commandHook_.Remove(clients);frameHook_.Remove(server);ConVar_Unregister();return true;
    }
    const char* GetAuthor() override {return "node2024";}
    const char* GetName() override {return "RouteLine";}
    const char* GetDescription() override {return "Private Global/SR replay route lines with timer provider separation";}
    const char* GetURL() override {return "";}
    const char* GetLicense() override {return "AGPLv3";}
    const char* GetVersion() override {return "0.16.19";}
    const char* GetDate() override {return __DATE__;}
    const char* GetLogTag() override {return "ROUTELINE";}
};
RouteLine plugin;
} // namespace
PLUGIN_EXPOSE(RouteLine,plugin);
CON_COMMAND_F(routeline,"Toggle replay route: [global|sr] [pro|overall] [ckz|vnl] [course]",FCVAR_CLIENT_CAN_EXECUTE) {
    plugin.run(context.GetPlayerSlot().Get(),args);
}
CON_COMMAND_F(routeline_reload,"Reload route settings and select a profile (server console/RCON)",FCVAR_NONE) {
    if(args.ArgC()>2) {META_CONPRINTF("Usage: routeline_reload [profile]\n");return;}
    plugin.reload(context.GetPlayerSlot().Get(),args.ArgC()>1?args.Arg(1):"");
}
