#pragma once
#include "replay.h"
#include <array>
#include <charconv>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace line {
using Json=nlohmann::json;
enum class DisplayDetail { Full, Compact, Landings };
enum class RenderBackend { Beam, Workshop };
inline constexpr size_t MaxFollowFrames=3000;
inline constexpr std::array<uint8_t,3> WorkshopLandingRgb{90,195,255};
inline constexpr std::array<uint8_t,3> DefaultCrouchRgb{255,40,40};
inline const char* backendName(RenderBackend backend) {
    return backend==RenderBackend::Workshop?"workshop":"beam";
}
struct DisplayLayers {
    bool path=true,jumps=true,landings=true;
    bool operator==(const DisplayLayers&) const = default;
};
struct Settings {
    std::string provider="cs2kz";
    std::string profile;
    std::vector<uint64_t> reloadAdmins;
    bool global=true,pro=true;
    float zOffset=2.5f,landingSize=20,width=0.5f,windowInterval=0.5f;
    float routeParticleSpacing=20.0f,markerParticleSpacing=8.0f;
    float routeDistance=0;
    std::optional<float> markerDistance;
    float landingWidth=0.85f;
    float pathBatchLength=1280;
    size_t pathBatchFrames=144;
    size_t totalElements=4096,createPerFrame=4,cacheRoutes=8;
    size_t replayFrames=MaxFollowFrames,windowOverlap=MaxFollowFrames/2;
    bool landingMarkers=true;
    bool takeoffMarkers=true;
    DisplayLayers layers;
    bool flatMarkers=false;
    bool meshMarkers=false; // Set when the map has loaded the Workshop marker models.
    bool packedRoute=false;
    bool follow=true;
    bool separateSections=true;
    DisplayDetail detail=DisplayDetail::Full;
    RenderBackend backend=RenderBackend::Workshop;
    // Route, landing, crouch RGBA values, kept as one array for old GCC's aggregate assignment.
    std::array<uint8_t,12> colors={255,255,255,255,40,140,255,255,
        DefaultCrouchRgb[0],DefaultCrouchRgb[1],DefaultCrouchRgb[2],255};
    DecodeOptions decode;
    bool useFlatMarkers() const {return landingMarkers&&flatMarkers&&detail==DisplayDetail::Full;}
    bool useTakeoffMarkers() const {return takeoffMarkers&&backend==RenderBackend::Workshop&&detail==DisplayDetail::Full;}
    float flatMarkerDistance() const {return markerDistance.value_or(std::min(routeDistance,600.0f));}
    bool canReloadFromChat(uint64_t steamId) const {
        return steamId&&std::find(reloadAdmins.begin(),reloadAdmins.end(),steamId)!=reloadAdmins.end();
    }
};
inline Settings readSettings(const std::string& file,const std::string& profile) {
    std::ifstream stream(file);
    if(!stream) throw std::runtime_error("Cannot open addons/routeline/config.json.");
    const auto root=Json::parse(stream);
    const auto& profiles=root.at("profiles");
    const auto fallback=profiles.contains("workshop-flat")?"workshop-flat":"cs2kz";
    const auto name=profile.empty()?root.value("active_profile",std::string(fallback)):profile;
    const auto& p=profiles.at(name);
    Settings s;
    s.profile=name;
    const auto admins=root.value("reload_admins",Json::array());
    if(!admins.is_array()) throw std::runtime_error("reload_admins must be an array of SteamID64 strings.");
    for(const auto& admin:admins) {
        if(!admin.is_string()) throw std::runtime_error("reload_admins entries must be SteamID64 strings.");
        const auto& value=admin.get_ref<const std::string&>();
        uint64_t id=0;
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),id);
        // Public-universe individual accounts with the standard SteamID64 instance.
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||(id>>32)!=0x01100001||!uint32_t(id))
            throw std::runtime_error("Invalid SteamID64 in reload_admins.");
        s.reloadAdmins.push_back(id);
    }
    s.provider=p.value("provider",s.provider);
    if(s.provider!="cs2kz") throw std::runtime_error("This build supports provider=cs2kz. Other backends need an adapter.");
    auto source=p.value("source",std::string("global")), category=p.value("category",std::string("pro"));
    if(source!="global"&&source!="sr") throw std::runtime_error("source must be global or sr.");
    if(category!="pro"&&category!="overall") throw std::runtime_error("category must be pro or overall.");
    s.global=source=="global";s.pro=category=="pro";
    const auto render=p.value("render",Json::object());
    if(render.value("mode",std::string("persistent"))!="persistent") throw std::runtime_error("render.mode must be persistent.");
    const auto renderer=render.value("renderer",std::string("workshop"));
    if(renderer=="workshop"||renderer=="model"||renderer=="beam"||renderer=="worldtext") {
        // Retired comparison profiles continue with the Workshop renderer.
        s.backend=RenderBackend::Workshop;
        s.landingSize=20;
        std::copy(WorkshopLandingRgb.begin(),WorkshopLandingRgb.end(),s.colors.begin()+4);
    }
    else throw std::runtime_error("render.renderer must be workshop.");
    s.zOffset=render.value("z_offset",s.zOffset);
    s.totalElements=render.value("max_total_elements",render.value("max_total_beams",s.totalElements));
    // Migrate a legacy nearby budget, which cannot hold a whole route. Explicit
    // persistent profiles retain their chosen cap, including small limits.
    if(!render.contains("mode")&&(render.contains("radius")||render.contains("segments_per_player")))
        s.totalElements=std::max(s.totalElements,size_t(4096));
    s.createPerFrame=render.value("create_per_frame",s.createPerFrame);
    s.landingMarkers=render.value("landing_markers",render.value("jump_markers",s.landingMarkers));
    s.takeoffMarkers=render.value("takeoff_markers",s.takeoffMarkers);
    if(render.value("markers_only",false)) {
        if(!s.landingMarkers) throw std::runtime_error("render.markers_only requires landing_markers=true.");
        s.layers={false,false,true};
    }
    const auto detail=render.value("detail",std::string("full"));
    if(detail=="compact") s.detail=DisplayDetail::Compact;
    else if(detail=="landings") s.detail=DisplayDetail::Landings;
    else if(detail!="full") throw std::runtime_error("render.detail must be full, compact or landings.");
    if(s.detail==DisplayDetail::Landings&&!s.landingMarkers)
        throw std::runtime_error("render.detail=landings requires landing_markers=true.");
    const auto markerRender=render.value("marker_render",std::string("rope"));
    if(markerRender=="flat") s.flatMarkers=true;
    else if(markerRender!="rope") throw std::runtime_error("render.marker_render must be rope or flat.");
    // landing_size is retired: old profiles also use fixed 20x20 markers.
    s.landingSize=20;
    s.landingWidth=render.value("landing_width",s.landingWidth);
    const auto routeBatch=render.value("route_batch",std::string("standard"));
    if(routeBatch=="packed") {
        s.packedRoute=true;
        s.pathBatchLength=1600;
        s.pathBatchFrames=320;
    } else if(routeBatch!="standard") throw std::runtime_error("render.route_batch must be standard or packed.");
    s.pathBatchLength=render.value("path_batch_length",s.pathBatchLength);
    if(auto frames=render.find("path_batch_frames");frames!=render.end()) {
        const auto maxFrames=s.packedRoute?512:256;
        if(!frames->is_number_integer()||*frames<1||*frames>maxFrames)
            throw std::runtime_error("render.path_batch_frames must be an integer from 1 to "+std::to_string(maxFrames)+".");
        s.pathBatchFrames=frames->get<size_t>();
    }
    s.width=render.value("beam_width",s.width);
    s.routeParticleSpacing=render.value("route_particle_spacing",s.routeParticleSpacing);
    s.routeDistance=render.value("route_distance",s.routeDistance);
    if(!std::isfinite(s.routeDistance)||s.routeDistance<0||s.routeDistance>8192)
        throw std::runtime_error("render.route_distance must be between 0 and 8192.");
    if(const auto distance=render.find("marker_distance");distance!=render.end()) {
        const auto value=distance->get<float>();
        if(!std::isfinite(value)||value<0||value>8192)
            throw std::runtime_error("render.marker_distance must be between 0 and 8192.");
        s.markerDistance=value;
    }
    s.markerParticleSpacing=render.value("marker_particle_spacing",s.markerParticleSpacing);
    s.replayFrames=render.value("replay_frames",s.replayFrames);
    s.follow=render.value("follow",s.follow);
    s.separateSections=render.value("separate_sections",s.separateSections);
    s.decode.hideFailedAttempts=render.value("hide_failed_attempts",s.decode.hideFailedAttempts);
    s.windowOverlap=render.value("window_overlap",s.replayFrames/2);
    s.windowInterval=render.value("window_interval",s.windowInterval);
    s.decode.pathXYError=render.value("path_xy_error",s.decode.pathXYError);
    s.decode.pathZError=render.value("path_z_error",s.decode.pathZError);
    s.decode.maxPathLength=render.value("max_path_length",s.decode.maxPathLength);
    const auto pathStyle=render.value("path_style",std::string("smooth"));
    if(pathStyle!="smooth"&&pathStyle!="precise") throw std::runtime_error("path_style must be smooth or precise.");
    s.decode.smoothPath=pathStyle=="smooth";
    s.decode.curveFlatness=render.value("curve_flatness",s.decode.curveFlatness);
    // Upgrade the stock 0.6 profile even when only the .so is replaced. Explicit
    // styles and custom error settings are preserved.
    if(!render.contains("path_style")&&s.decode.pathXYError==6&&s.decode.pathZError==1&&s.decode.maxPathLength==128) {
        s.decode.pathZError=3;s.decode.maxPathLength=256;
    }
    s.cacheRoutes=p.value("cache_routes",s.cacheRoutes);
    auto readColor=[&](const char* name,uint8_t* output) {
        if(!render.contains(name)) return;
        const auto& c=render.at(name);
        if(!c.is_array()||(c.size()!=3&&c.size()!=4)) throw std::runtime_error("Colors must be [r,g,b] or [r,g,b,a].");
        for(size_t i=0;i<c.size();++i) {int v=c[i].get<int>();if(v<0||v>255) throw std::runtime_error("Color component out of range.");output[i]=uint8_t(v);}
    };
    readColor("color",s.colors.data());readColor("route_color",s.colors.data());
    readColor("jump_color",s.colors.data()+4);readColor("landing_color",s.colors.data()+4);readColor("crouch_color",s.colors.data()+8);
    const auto limits=p.value("limits",Json::object());
    const auto fileMB=limits.value("max_file_mb",size_t(128)),decodedMB=limits.value("max_decoded_mb",size_t(256));
    if(!fileMB||fileMB>512||!decodedMB||decodedMB>512) throw std::runtime_error("Replay size limit must be between 1 and 512 MiB.");
    s.decode.maxFileBytes=fileMB*1024*1024;
    s.decode.maxDecodedBytes=decodedMB*1024*1024;
    s.decode.maxTicks=limits.value("max_ticks",s.decode.maxTicks);
    s.decode.maxSegments=limits.value("max_segments",s.decode.maxSegments);
    s.decode.maxLandings=limits.value("max_landings",limits.value("max_jumps",s.decode.maxLandings));
    s.decode.spacing=limits.value("point_spacing",s.decode.spacing);
    s.decode.maxStep=limits.value("max_step",s.decode.maxStep);
    s.decode.lineError=limits.value("line_error",s.decode.lineError);
    auto finite=[](float v,float lo,float hi) { return std::isfinite(v)&&v>=lo&&v<=hi; };
    if(!finite(s.zOffset,-32,64)||!finite(s.landingSize,8,128)||!finite(s.width,0.1f,4)||!finite(s.landingWidth,0.5f,1.25f)
       ||!finite(s.pathBatchLength,640,s.packedRoute?2048.0f:1280.0f)||!finite(s.routeParticleSpacing,1,100)||!finite(s.markerParticleSpacing,1,8)
       ||!s.replayFrames||s.replayFrames>MaxFollowFrames||s.windowOverlap>=s.replayFrames||!finite(s.windowInterval,0.1f,2)
       ||!finite(s.decode.pathXYError,0.1f,16)||!finite(s.decode.pathZError,0.1f,4)||!finite(s.decode.maxPathLength,32,512)
       ||!finite(s.decode.curveFlatness,0.25f,8)
       ||!s.totalElements||s.totalElements>12000||!s.createPerFrame||s.createPerFrame>128||!s.cacheRoutes||s.cacheRoutes>32
       ||s.decode.maxFileBytes<1024*1024||s.decode.maxFileBytes>512ULL*1024*1024
       ||s.decode.maxDecodedBytes<1024*1024||s.decode.maxDecodedBytes>512ULL*1024*1024
       ||!s.decode.maxTicks||s.decode.maxTicks>4000000||!s.decode.maxSegments||s.decode.maxSegments>500000
       ||!s.decode.maxLandings||s.decode.maxLandings>200000||!finite(s.decode.lineError,0,8)
       ||!finite(s.decode.spacing,1,64)||!finite(s.decode.maxStep,32,1024)) throw std::runtime_error("Render or replay limits are out of range.");
    return s;
}
} // namespace line
