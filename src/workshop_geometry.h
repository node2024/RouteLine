#pragma once

#include "drawing.h"
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>

namespace line {
inline constexpr const char* WorkshopRouteEffect="particles/routeline/route.vpcf";
inline constexpr const char* WorkshopMarkerEffect="particles/routeline/marker.vpcf";
inline constexpr const char* WorkshopRouteFile="particles/routeline/route.vpcf_c";
inline constexpr const char* WorkshopMarkerFile="particles/routeline/marker.vpcf_c";
struct WorkshopAsset {const char* effect;const char* file;};
inline constexpr WorkshopAsset WorkshopFlatMarkerCustom{
    "particles/routeline/marker_flat.vpcf","particles/routeline/marker_flat.vpcf_c"};
inline constexpr WorkshopAsset WorkshopFlatMarkerBlue{
    "particles/routeline/marker_flat_blue.vpcf","particles/routeline/marker_flat_blue.vpcf_c"};
inline constexpr WorkshopAsset WorkshopFlatMarkerRed{
    "particles/routeline/marker_flat_red.vpcf","particles/routeline/marker_flat_red.vpcf_c"};
inline constexpr const char* WorkshopFlatMarkerTexture="materials/routeline/marker_flat.vtex_c";
inline constexpr WorkshopAsset WorkshopJumpAmber{
    "particles/routeline/jump_flat_amber.vpcf","particles/routeline/jump_flat_amber.vpcf_c"};
inline constexpr WorkshopAsset WorkshopJumpGreen{
    "particles/routeline/jump_flat_green.vpcf","particles/routeline/jump_flat_green.vpcf_c"};
inline constexpr const char* WorkshopJumpTexture="materials/routeline/jump_flat.vtex_c";
inline constexpr WorkshopAsset WorkshopLandingMesh{
    "models/routeline/landing_mark.vmdl","models/routeline/landing_mark.vmdl_c"};
inline constexpr WorkshopAsset WorkshopJumpMesh{
    "models/routeline/jump_mark.vmdl","models/routeline/jump_mark.vmdl_c"};
inline constexpr const char* WorkshopMarkerMaterial="materials/routeline/marker_mesh.vmat_c";
inline constexpr const char* WorkshopMarkerMaterialTexture="materials/routeline/marker_mesh_vmat_g_tcolor_19031fd4.vtex_c";
inline constexpr std::array<uint8_t,4> WorkshopJumpAmberColor{255,196,60,255};
inline constexpr std::array<uint8_t,4> WorkshopJumpGreenColor{80,255,105,255};

inline const WorkshopAsset& workshopFlatMarkerAsset(const uint8_t* color) {
    if(std::equal(WorkshopLandingRgb.begin(),WorkshopLandingRgb.end(),color)) return WorkshopFlatMarkerBlue;
    if(std::equal(DefaultCrouchRgb.begin(),DefaultCrouchRgb.end(),color)) return WorkshopFlatMarkerRed;
    return WorkshopFlatMarkerCustom;
}

struct GroundHit {Vec3 point,normal;};

// Keep the nearby-ground position correction, but never resize or tilt the frame.
// Its edges may extend beyond the support. No support means recorded-position fallback.
template<class Probe>
inline Vec3 workshopMarkerCenter(Vec3 origin,const Settings& settings,Probe probe,bool ladderContact=false) {
    if(ladderContact) return origin+Vec3{0,0,settings.zOffset};
    auto hit=probe(origin);
    constexpr std::array<Vec3,8> directions={{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},
        {1,1,0},{1,-1,0},{-1,1,0},{-1,-1,0}}};
    for(float distance:{4.0f,8.0f,12.0f,16.0f}) {
        if(hit) break;
        for(const auto direction:directions) {
            hit=probe(origin+direction*distance);
            if(hit) break;
        }
    }
    return hit?hit->point+Vec3{0,0,2.5f}:origin+Vec3{0,0,settings.zOffset};
}
template<class Probe>
inline std::vector<Vec3> workshopMarker(Vec3 origin,const Settings& settings,Probe probe,bool ladderContact=false) {
    const Vec3 center=workshopMarkerCenter(origin,settings,probe,ladderContact);
    if(settings.detail!=DisplayDetail::Full)
        return {center-Vec3{4,0,0},center+Vec3{4,0,0}};
    const auto edges=landingOutline(center,20);
    std::vector<Vec3> nodes;
    for(const auto& edge:edges) nodes.push_back(edge.start);
    nodes.push_back(nodes.front());
    return nodes;
}

// Compiled Hammer paths store local XYZ, incoming XYZ and outgoing XYZ as
// a KV3 array in the pathNodes string. Use linear Bezier handles so the
// checked, simplified route is retained through hard corners.
inline std::string workshopPathNodes(const std::vector<Vec3>& points) {
    std::ostringstream nodes;
    nodes.imbue(std::locale::classic());
    nodes<<std::fixed<<std::setprecision(5)<<'[';
    auto vector=[&](Vec3 v) {nodes<<v.x<<','<<v.y<<','<<v.z;};
    for(size_t i=0;i<points.size();++i) {
        if(i) nodes<<',';
        nodes<<'[';
        vector(points[i]-points.front());
        nodes<<',';
        vector(i?(points[i-1]-points[i])*(1.0f/3):Vec3{});
        nodes<<',';
        vector(i+1<points.size()?(points[i+1]-points[i])*(1.0f/3):Vec3{});
        nodes<<']';
    }
    nodes<<']';
    return nodes.str();
}

inline std::string workshopPinnedNodes(size_t count) {
    std::string pins="[";
    for(size_t i=0;i<count;++i) pins+=i?",true":"true";
    return pins+"]";
}
inline std::string workshopNodeColors(const std::vector<bool>& crouched,const Settings& settings) {
    std::ostringstream nodes;
    nodes.imbue(std::locale::classic());
    nodes<<std::fixed<<std::setprecision(6)<<'[';
    for(size_t i=0;i<crouched.size();++i) {
        if(i) nodes<<',';
        const auto* color=primitiveColor(crouched[i]?PrimitiveKind::Crouch:PrimitiveKind::Route,settings);
        nodes<<'['<<color[0]/255.0f<<','<<color[1]/255.0f<<','<<color[2]/255.0f<<']';
    }
    nodes<<']';
    return nodes.str();
}
} // namespace line
