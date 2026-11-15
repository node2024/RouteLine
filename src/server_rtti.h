#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <link.h>
#include <string>
#include <string_view>
#include <vector>

namespace line {
// Locate the primary Itanium-ABI vtable in libserver's relocated data, even
// when RTTI symbols are not exported. Reject ambiguous or non-code entries.
inline void** findServerVTable(std::string_view className,size_t requiredSlots) {
    struct Region {
        uintptr_t first,last;
        bool executable;
    };
    std::vector<Region> regions;
    dl_iterate_phdr([](dl_phdr_info* info,size_t,void* context) {
        if(!std::string_view(info->dlpi_name).ends_with("/libserver.so")) return 0;
        auto& regions=*static_cast<std::vector<Region>*>(context);
        for(size_t i=0;i<info->dlpi_phnum;++i) {
            const auto& header=info->dlpi_phdr[i];
            if(header.p_type!=PT_LOAD||!(header.p_flags&PF_R)||!header.p_memsz) continue;
            const uintptr_t first=info->dlpi_addr+header.p_vaddr;
            regions.push_back({first,first+header.p_memsz,bool(header.p_flags&PF_X)});
        }
        // Metamod's loader is also named libserver.so. Keep scanning for the
        // actual game module, which owns the named RTTI and vtable.
        return 0;
    },&regions);

    const auto name=std::to_string(className.size())+std::string(className);
    uintptr_t typeName=0;
    for(const auto& region:regions) {
        if(region.executable||region.last-region.first<=name.size()) continue;
        const char* start=reinterpret_cast<const char*>(region.first);
        const char* end=reinterpret_cast<const char*>(region.last);
        for(const char* found=start; (found=std::search(found,end,name.c_str(),name.c_str()+name.size()+1))!=end; ++found) {
            if(found!=start&&found[-1]!='\0') continue;
            if(typeName) return nullptr;
            typeName=reinterpret_cast<uintptr_t>(found);
        }
    }
    if(!typeName) return nullptr;

    auto read=[](uintptr_t address) {
        uintptr_t word;
        std::memcpy(&word,reinterpret_cast<const void*>(address),sizeof(word));
        return word;
    };
    constexpr size_t word=sizeof(uintptr_t);
    std::vector<uintptr_t> typeInfos;
    for(const auto& region:regions) {
        if(region.executable) continue;
        for(auto address=region.first+word;address+word<=region.last;address+=word)
            if(read(address)==typeName&&read(address-word)!=0) typeInfos.push_back(address-word);
    }
    if(typeInfos.empty()) return nullptr;

    void** table=nullptr;
    for(const auto& region:regions) {
        if(region.executable) continue;
        for(auto address=region.first+word;address+(requiredSlots+1)*word<=region.last;address+=word) {
            if(read(address-word)!=0||std::find(typeInfos.begin(),typeInfos.end(),read(address))==typeInfos.end()) continue;
            bool valid=true;
            // Other plugins can already own hooks in later slots. The primary
            // destructor entries identify the original server vtable.
            for(size_t slot=0;slot<std::min(requiredSlots,size_t(2));++slot) {
                const auto function=read(address+(slot+1)*word);
                if(!std::any_of(regions.begin(),regions.end(),[&](const Region& code) {
                    return code.executable&&function>=code.first&&function<code.last;
                })) {
                    valid=false;
                    break;
                }
            }
            if(!valid) continue;
            if(table) return nullptr;
            table=reinterpret_cast<void**>(address+word);
        }
    }
    return table;
}
} // namespace line
