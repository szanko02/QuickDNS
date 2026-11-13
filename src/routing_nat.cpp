#include "routing_nat.h"
#include <algorithm>
#include <cstddef>
namespace quickdns {
namespace {
uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>((p[0]<<8)|p[1]); }
void put16(uint8_t* p, uint16_t n) { p[0]=static_cast<uint8_t>(n>>8); p[1]=static_cast<uint8_t>(n); }
uint32_t add(const uint8_t* p, size_t n, uint32_t sum=0) {
    for(size_t i=0;i<n;i+=2) sum+=(uint32_t(p[i])<<8)|(i+1<n?p[i+1]:0);
    return sum;
}
uint16_t finish(uint32_t sum) { while(sum>>16) sum=(sum&0xffff)+(sum>>16); return static_cast<uint16_t>(~sum); }
}
bool rewrite_address(std::span<uint8_t> p, std::span<const uint8_t> a, bool source) {
    if(p.empty()) return false;
    const auto ver=p[0]>>4;
    size_t hdr=0, addr=0, addrlen=0, checksum=0, transport_len=0;
    uint8_t protocol=0;
    if(ver==4) {
        if(p.size()<20 || (p[0]&15)!=5 || a.size()!=4 || read16(p.data()+2)!=p.size() || (read16(p.data()+6)&0x3fff)!=0) return false;
        hdr=20; addr=source?12:16; addrlen=4; protocol=p[9]; transport_len=p.size()-hdr;
    } else if(ver==6) {
        if(p.size()<40 || a.size()!=16 || read16(p.data()+4)!=p.size()-40) return false;
        hdr=40; addr=source?8:24; addrlen=16; protocol=p[6]; transport_len=p.size()-hdr;
    } else return false;
    if(protocol==6) {
        if(transport_len<20 || (p[hdr+12]>>4)<5 || size_t(p[hdr+12]>>4)*4>transport_len) return false;
        checksum=16;
    } else if(protocol==17) {
        if(transport_len<8 || read16(p.data()+hdr+4)!=transport_len) return false;
        checksum=6;
    } else return false;
    std::copy(a.begin(),a.end(),p.begin()+addr);
    if(ver==4) { p[10]=p[11]=0; put16(p.data()+10,finish(add(p.data(),hdr))); }
    p[hdr+checksum]=p[hdr+checksum+1]=0;
    uint32_t s=add(p.data()+(ver==4?12:8),addrlen*2);
    s+=static_cast<uint32_t>(transport_len)+protocol;
    uint16_t result=finish(add(p.data()+hdr,transport_len,s));
    if(protocol==17 && result==0) result=0xffff;
    put16(p.data()+hdr+checksum,result);
    return true;
}
}
