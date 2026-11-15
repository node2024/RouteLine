#pragma once
#include "drawing.h"
#include <optional>
#include <set>

namespace line {
// A route's geometry is immutable. Each element tracks the players whose
// selection includes it, so a window change can keep the overlapping visuals.
template<class Visual> class PersistentDisplays {
public:
    struct Entry {uint64_t subscribers=0;std::optional<Visual> visual;};
    struct Group {
        std::shared_ptr<const Route> route;
        std::vector<Primitive> drawing;
        std::vector<Entry> entries;
        std::set<size_t> active;
        uint64_t subscribers=0;
        size_t cursor=0;
        int privateSlot=-1;
        Settings settings;
    };
private:
    struct NearbySelection {
        Vec3 position;
        float routeDistance,markerDistance;
        std::vector<size_t> candidates;
    };
    struct Owner {
        std::shared_ptr<Group> group;
        std::vector<size_t> selected;
        FrameWindow window,visibleWindow;
        std::optional<NearbySelection> nearby;
        std::optional<size_t> preview,previewMarker;
        DisplayLayers layers;
    };
    std::array<Owner,64> owners_{};
    std::vector<std::shared_ptr<Group>> groups_;
    size_t reserved_=0;
public:
    const auto& groups() const {return groups_;}
    const Group* group(int slot) const {return slot>=0&&slot<64?owners_[slot].group.get():nullptr;}
    const auto& selected(int slot) const {return owners_.at(slot).selected;}
    FrameWindow window(int slot) const {return owners_.at(slot).visibleWindow;}
    size_t reserved() const {return reserved_;}
    template<class Erase> void clear(int slot,Erase erase) {
        auto& owner=owners_.at(slot);auto group=owner.group;if(!group) return;
        const auto bit=uint64_t(1)<<slot;
        for(auto i:owner.selected) {
            auto& entry=group->entries[i];entry.subscribers&=~bit;
            if(!entry.subscribers) {--reserved_;group->active.erase(i);if(entry.visual) {erase(*entry.visual);entry.visual.reset();}}
        }
        group->subscribers&=~bit;owner={};
        if(!group->subscribers) groups_.erase(std::remove(groups_.begin(),groups_.end(),group),groups_.end());
    }
    template<class Erase> void attach(int slot,std::shared_ptr<const Route> route,Vec3 position,const Settings& settings,Erase erase) {
        if(slot<0||slot>=64||!route) throw std::runtime_error("Invalid route subscriber.");
        std::shared_ptr<Group> target;
        // Each Workshop effect needs its own stop signal when its viewer leaves.
        const int privateSlot=settings.backend==RenderBackend::Workshop?slot:-1;
        for(const auto& g:groups_) if(g->route==route&&g->privateSlot==privateSlot&&g->settings.backend==settings.backend) {target=g;break;}
        if(!target) {
            target=std::make_shared<Group>();target->route=route;target->drawing=buildDrawing(*route,settings);
            target->privateSlot=privateSlot;
            target->settings=settings;
            if(target->drawing.empty()) throw std::runtime_error("Replay has nothing to draw in this mode. Current display kept.");
            target->entries.resize(target->drawing.size());
        }
        auto& owner=owners_[slot];const auto bit=uint64_t(1)<<slot;
        const auto current=owner.group==target?owner.window:FrameWindow{};
        const size_t nearest=settings.follow?route->nearestFrame(position):0;
        const auto window=drawingWindowAtFrame(*route,nearest,settings,current);
        auto visibleWindow=window;
        const bool sameWindow=owner.group==target&&window==current;
        const bool sameMode=owner.layers==settings.layers;
        const float routeDistance=settings.routeDistance;
        const float markerDistance=settings.flatMarkerDistance();
        const bool nearby=!window.all&&(routeDistance>0||markerDistance>0)&&position.valid();
        if(sameWindow&&sameMode&&!nearby&&!owner.nearby) return;
        const bool reuseCandidates=sameWindow&&nearby&&owner.nearby.has_value();
        if(reuseCandidates&&sameMode&&routeDistance==owner.nearby->routeDistance&&markerDistance==owner.nearby->markerDistance) {
            const float movementThreshold=std::min({32.0f,routeDistance>0?routeDistance/4:32.0f,
                markerDistance>0?markerDistance/4:32.0f});
            if(distance2(position,owner.nearby->position)<movementThreshold*movementThreshold) return;
        }
        std::vector<size_t> candidates,desired;
        if(reuseCandidates) desired=owner.nearby->candidates;
        else {
            desired=selectDrawing(target->drawing,window);
            if(nearby) candidates=desired;
        }
        std::optional<size_t> preview,previewMarker;
        const auto isPath=[&](size_t i) {return isRoutePath(target->drawing[i].kind);};
        const auto hidden=[&](size_t i) {
            const auto kind=target->drawing[i].kind;
            if(isRoutePath(kind)) return !settings.layers.path;
            return kind==PrimitiveKind::Landing?!settings.layers.landings:!settings.layers.jumps;
        };
        const auto reservedAfter=[&](const std::vector<size_t>& selection) {
            size_t released=0,needed=0;
            if(owner.group) for(auto i:owner.selected)
                if(owner.group->entries[i].subscribers==bit
                   &&(owner.group!=target||!std::binary_search(selection.begin(),selection.end(),i))) ++released;
            for(auto i:selection) if(!target->entries[i].subscribers) ++needed;
            return reserved_-released+needed;
        };
        const Vec3 pathPosition=position-Vec3{0,0,settings.zOffset};
        if(nearby&&(settings.layers.path||settings.layers.landings||settings.layers.jumps)) {
            std::erase_if(desired,[&](size_t index) {
                const auto& path=target->drawing[index];
                const bool line=isPath(index);
                const float distance=line?routeDistance:markerDistance;
                if(distance<=0) return false;
                // A preview earns the retention margin only after entering the regular radius.
                const bool retained=(target->entries[index].subscribers&bit)!=0&&owner.preview!=index&&owner.previewMarker!=index;
                const float radius=distance+(retained?std::min(128.0f,distance/4):0);
                if(!line) {
                    return !path.flatMarkerOrigins.empty()&&!markerInRange(path,pathPosition,radius);
                }
                return !routePathInRange(path,pathPosition,radius);
            });
            if(const auto next=routeDistance>0?followContinuation(target->drawing,desired,nearest,pathPosition,settings):std::nullopt) {
                const auto& path=target->drawing[next->index];
                if(next->overlap==FollowOverlap::None) preview=next->index;
                else if(settings.separateSections&&path.firstFrame>=window.end&&path.firstFrame<route->frames.size()) {
                    // The later pass continues the replay. Replace the nearby earlier
                    // pass instead of drawing both or waiting for a closer frame.
                    const auto nextSection=route->frames[path.firstFrame].section;
                    const bool checkpointReturn=std::binary_search(route->checkpointSections.begin(),
                        route->checkpointSections.end(),nextSection);
                    const auto nextWindow=drawingWindowAtFrame(*route,path.firstFrame,settings);
                    if((next->overlap==FollowOverlap::SameDirection||checkpointReturn)&&path.lastFrame<nextWindow.end) {
                        auto nextPaths=selectDrawing(target->drawing,nextWindow);
                        std::erase_if(nextPaths,[&](size_t i) {
                            const auto& upcoming=target->drawing[i];
                            if(!isPath(i)) return settings.layers.path||hidden(i)
                                ||(markerDistance>0&&!markerInRange(upcoming,pathPosition,markerDistance));
                            return !routePathInRange(upcoming,pathPosition,routeDistance);
                        });
                        if(std::any_of(nextPaths.begin(),nextPaths.end(),[&](size_t i) {
                            return routePathInRange(target->drawing[i],pathPosition,std::min(128.0f,routeDistance));
                        })) {
                            std::erase_if(nextPaths,hidden);
                            if(reservedAfter(nextPaths)<=settings.totalElements) {
                                visibleWindow=nextWindow;
                                desired=std::move(nextPaths);
                            }
                        }
                    }
                }
            }
        }
        std::erase_if(desired,hidden);
        auto rememberSelection=[&] {
            owner.window=window;
            owner.visibleWindow=visibleWindow;
            owner.preview=preview;
            owner.previewMarker=previewMarker;
            owner.layers=settings.layers;
            if(!nearby) owner.nearby.reset();
            else if(reuseCandidates) {
                owner.nearby->position=position;
                owner.nearby->routeDistance=routeDistance;
                owner.nearby->markerDistance=markerDistance;
            } else owner.nearby=NearbySelection{position,routeDistance,markerDistance,std::move(candidates)};
        };
        if(!preview&&owner.group==target&&owner.selected==desired) {rememberSelection();return;}
        const auto baseReserved=reservedAfter(desired);
        if(baseReserved>settings.totalElements)
            throw std::runtime_error("Replay window needs "+std::to_string(desired.size())+" elements; max_total_elements="
                +std::to_string(settings.totalElements)+" is full. Current display kept.");
        if(preview) {
            const auto otherSubscribers=target->entries[*preview].subscribers&~bit;
            const auto pathCost=size_t(settings.layers.path&&!otherSubscribers);
            if(baseReserved+pathCost<=settings.totalElements) {
                const auto& path=target->drawing[*preview];
                visibleWindow.end=std::max(visibleWindow.end,size_t(path.lastFrame)+1);
                // Make room at the back if the preview extends a full page.
                visibleWindow.begin=std::max(visibleWindow.begin,visibleWindow.end-std::min(visibleWindow.end,settings.replayFrames));
                std::erase_if(desired,[&](size_t i){return target->drawing[i].firstFrame<visibleWindow.begin;});
                if(settings.layers.path) desired.insert(std::lower_bound(desired.begin(),desired.end(),*preview),*preview);
                if(settings.layers.landings&&baseReserved+pathCost<settings.totalElements) {
                    const float markerPreviewDistance=markerDistance>0?std::min(markerDistance,routeDistance):routeDistance;
                    previewMarker=followPreviewMarker(target->drawing,desired,path,visibleWindow,pathPosition,
                        followPreviewDistance(markerPreviewDistance));
                    if(previewMarker) desired.insert(std::lower_bound(desired.begin(),desired.end(),*previewMarker),*previewMarker);
                }
            } else preview.reset();
        }
        if(owner.group==target&&owner.selected==desired) {rememberSelection();return;}
        // Retain the overlap before dropping the old window, so native handles stay
        // stable and are not destroyed/recreated when the nearest frame changes.
        for(auto i:desired) {
            auto& entry=target->entries[i];
            if(!(entry.subscribers&bit)) {if(!entry.subscribers) {++reserved_;target->active.insert(i);}entry.subscribers|=bit;}
        }
        if(owner.group) {
            const auto old=owner.group;
            for(auto i:owner.selected) if(old!=target||!std::binary_search(desired.begin(),desired.end(),i)) {
                auto& entry=old->entries[i];entry.subscribers&=~bit;
                if(!entry.subscribers) {--reserved_;old->active.erase(i);if(entry.visual) {erase(*entry.visual);entry.visual.reset();}}
            }
            if(old!=target) {
                old->subscribers&=~bit;
                if(!old->subscribers) groups_.erase(std::remove(groups_.begin(),groups_.end(),old),groups_.end());
            }
        }
        if(!target->subscribers) groups_.push_back(target);
        target->subscribers|=bit;
        owner.group=target;
        owner.selected=std::move(desired);
        rememberSelection();
    }
};
} // namespace line
