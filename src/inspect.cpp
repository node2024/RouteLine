#include "replay.h"
#include "drawing.h"
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc<5||argc>7) {std::cerr<<"Usage: replay_inspect FILE MAP COURSE classic|vanilla [pro|overall] [smooth|precise]\n";return 2;}
    try {
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        const auto n=file.tellg();line::DecodeOptions options;
        if(!file||n<=0||uint64_t(n)>options.maxFileBytes) throw std::runtime_error("Cannot read file or file exceeds the size limit.");
        std::string bytes(size_t(n),'\0');file.seekg(0);if(!file.read(bytes.data(),n)) throw std::runtime_error("Cannot read file.");
        line::Query q;q.map=argv[2];q.course=argv[3];q.mode=argv[4];q.pro=argc>=6&&std::string(argv[5])=="pro";
        if(argc==7) {options.smoothPath=std::string(argv[6])!="precise";if(!options.smoothPath) {options.pathZError=1;options.maxPathLength=128;}}
        auto route=line::decodeReplay(bytes,q,{},options);
        const auto duck=std::count_if(route.segments.begin(),route.segments.end(),[](const line::Segment& s){return s.crouched;});
        const auto perfect=std::count_if(route.jumps.begin(),route.jumps.end(),[](const line::Jump& j){return j.perfect;});
        std::cout<<"version="<<route.replayVersion<<" segments="<<route.segments.size()<<" crouch_segments="<<duck<<" curves="<<route.pathCurves<<" path_segments="<<route.path.size()
                 <<" landings="<<route.landings.size()<<" inferred="<<route.recoveredLandings<<" frames="<<route.frames.size()
                 <<" jumps="<<route.jumps.size()<<" inferred_perfect="<<perfect<<"\n";
        line::Settings settings;
        for(const auto& [detail,name]:{std::pair{line::DisplayDetail::Full,"full"},std::pair{line::DisplayDetail::Compact,"compact"},std::pair{line::DisplayDetail::Landings,"landings"}}) {
            settings.detail=detail;
            const auto drawing=line::buildDrawing(route,settings);
            auto count=[&](size_t frame) {return route.frames.empty()?size_t(0):line::selectDrawing(drawing,line::drawingWindow(route,route.frames[frame].position,settings)).size();};
            std::cout<<name<<" paths="<<drawing.size()<<" selected_start="<<count(0)<<" selected_mid="<<count(route.frames.size()/2)
                     <<" selected_end="<<count(route.frames.empty()?0:route.frames.size()-1)<<"\n";
        }
        settings.backend=line::RenderBackend::Workshop;
        settings.detail=line::DisplayDetail::Full;
        const auto workshop=line::buildDrawing(route,settings);
        auto countPaths=[&](size_t frame) {
            if(route.frames.empty()) return size_t(0);
            return line::selectDrawing(workshop,line::drawingWindow(route,route.frames[frame].position,settings)).size();
        };
        std::cout<<"workshop paths="<<workshop.size()<<" selected_start="<<countPaths(0)
                 <<" selected_mid="<<countPaths(route.frames.size()/2)
                 <<" selected_end="<<countPaths(route.frames.empty()?0:route.frames.size()-1)<<"\n";
        return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
