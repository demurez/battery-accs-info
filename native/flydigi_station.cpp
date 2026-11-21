#define NOMINMAX
#include <windows.h>
#include "flydigi_station.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace {
using Bytes = std::span<const unsigned char>;
bool integer(Bytes& bytes, uint64_t& value) {
    value=0;
    for (unsigned shift=0; shift<64 && !bytes.empty(); shift+=7) {
        unsigned b=bytes.front();
        bytes=bytes.subspan(1);
        if (shift==63 && b>1) return false;
        value |= uint64_t(b&127)<<shift;
        if (!(b&128)) return true;
    }
    return false;
}
struct Field { unsigned number{},wire{}; uint64_t value{}; Bytes data; };
bool next(Bytes& bytes,Field& f) {
    uint64_t tag{},width{};
    if (!integer(bytes,tag) || !tag || tag>UINT32_MAX) return false;
    f={unsigned(tag>>3),unsigned(tag&7)};
    switch(f.wire) {
    case 0: return integer(bytes,f.value);
    case 1: width=8; break;
    case 2: if (!integer(bytes,width)) return false; break;
    case 5: width=4; break;
    default: return false;
    }
    if(width>bytes.size())return false;
    f.data=bytes.first(size_t(width)); bytes=bytes.subspan(size_t(width)); return true;
}
std::optional<Reading> device(Bytes bytes) {
    bool model=false,connected=false;
    uint64_t level=UINT64_MAX;
    while(!bytes.empty()){
        Field f;
        if(!next(bytes,f))return {};
        if(f.number==4 && f.wire==2) model=std::string_view(reinterpret_cast<const char*>(f.data.data()),f.data.size())=="fp4";
        if(f.number==8 && f.wire==0) connected=f.value!=0;
        if(f.number==9 && f.wire==0) level=f.value;
    }
    if(!model || !connected || level>6)return {};
    if(level==6)return Reading{{},L"Заряжается"};
    constexpr int values[]{5,20,40,60,80,100};
    return Reading{values[level],L"По данным Space Station",true};
}
std::optional<Reading> response(Bytes bytes) {
    uint64_t command=0;
    Bytes devices,update;
    while(!bytes.empty()){
        Field f;
        if(!next(bytes,f))return {};
        if(f.number==1 && !f.wire)command=f.value;
        if(f.number==5 && f.wire==2)devices=f.data;
        if(f.number==6 && f.wire==2)update=f.data;
    }
    Bytes list=command==4097 && !devices.empty()?devices:update;
    while(!list.empty()){
        Field f;
        if(!next(list,f))return {};
        if(f.number==1 && f.wire==2)if(auto reading=device(f.data))return reading;
    }
    return {};
}
}
std::optional<Reading> FlydigiStationBattery() {
    HANDLE h=CreateFileW(L"\\\\.\\pipe\\fcs.sock",GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                         SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
    if(h==INVALID_HANDLE_VALUE)return {};
    struct Close {HANDLE h; ~Close(){CloseHandle(h);}} close{h};
    constexpr unsigned char request[]="FDG_PROTOCOL\n\x05\0\0\0\x08\x81\x20\x1a\0";
    DWORD size{};
    if(!WriteFile(h,request,sizeof(request)-1,&size,nullptr)||size!=sizeof(request)-1)return {};
    const auto end=GetTickCount64()+1500;
    auto read=[&](unsigned char* out,DWORD remaining){
        while(remaining && GetTickCount64()<end){
            DWORD avail{},got{};
            if(!PeekNamedPipe(h,nullptr,0,nullptr,&avail,nullptr))return false;
            if(!avail){Sleep(10);continue;}
            if(!ReadFile(h,out,std::min(avail,remaining),&got,nullptr)||!got)return false;
            remaining-=got;out+=got;
        }
        return remaining==0;
    };
    while(GetTickCount64()<end){
        unsigned char header[17]{};
        if(!read(header,sizeof(header))||memcmp(header,request,13))return {};
        DWORD length{};
        memcpy(&length,header+13,4);
        if(length>1024*1024)return {};
        std::vector<unsigned char> packet(length);
        if(!read(packet.data(),length))return {};
        if(auto result=response(packet))return result;
    }
    return {};
}
