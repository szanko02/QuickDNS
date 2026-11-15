#include "routing_nat.h"
#include <array>
#include <vector>
#include <cstdint>
using namespace quickdns;
static uint32_t sum(const uint8_t* p,size_t n,uint32_t s=0) { for(size_t i=0;i<n;i+=2) s+=(uint32_t(p[i])<<8)|(i+1<n?p[i+1]:0); while(s>>16)s=(s&65535)+(s>>16); return s; }
static bool verify(const std::vector<uint8_t>& p) {
 if((p[0]>>4)==4) { if(sum(p.data(),20)!=65535) return false; auto s=sum(p.data()+12,8); s+=p[9]+(p.size()-20); return sum(p.data()+20,p.size()-20,s)==65535; }
 auto s=sum(p.data()+8,32); s+=p[6]+(p.size()-40); return sum(p.data()+40,p.size()-40,s)==65535;
}
int main() {
 std::vector<uint8_t> v4={0x45,0,0,28,0,1,0,0,64,17,0,0,10,0,0,1,8,8,8,8,0x30,0x39,0,53,0,8,0,0};
 std::array<uint8_t,4> target={192,0,2,1}; if(!rewrite_address(v4,target,true)||!verify(v4)||v4[12]!=192) return 1;
 std::array<uint8_t,4> original={10,0,0,1}; if(!rewrite_address(v4,original,false)||!verify(v4)||v4[16]!=10) return 2;
 auto bad=v4; bad[6]=0x20; auto before=bad; if(rewrite_address(bad,target,true)||bad!=before) return 3;
 std::vector<uint8_t> v6(60); v6[0]=0x60; v6[5]=20; v6[6]=6; v6[7]=64; v6[8]=0x20; v6[24]=0x20; v6[40]=0x30; v6[41]=0x39; v6[42]=1; v6[43]=187; v6[52]=0x50;
 std::array<uint8_t,16> new6{}; new6[0]=0x20; new6[15]=1;
 if(!rewrite_address(v6,new6,true)||!verify(v6)||v6[23]!=1) return 4;
 auto extension=v6; extension[6]=44; before=extension; if(rewrite_address(extension,new6,true)||extension!=before) return 5;
  std::vector<uint8_t> tcp4(41); tcp4[0]=0x45; tcp4[3]=41; tcp4[8]=64; tcp4[9]=6; tcp4[12]=10; tcp4[16]=8; tcp4[32]=0x50; tcp4[40]=42;
 if(!rewrite_address(tcp4,target,true)||!verify(tcp4)) return 7;
 std::vector<uint8_t> udp6(49); udp6[0]=0x60; udp6[5]=9; udp6[6]=17; udp6[8]=0x20; udp6[24]=0x20; udp6[45]=9; udp6[48]=42;
 if(!rewrite_address(udp6,new6,false)||!verify(udp6)) return 8;
 auto invalid_udp=udp6; invalid_udp[45]=8; before=invalid_udp; if(rewrite_address(invalid_udp,new6,true)||invalid_udp!=before) return 9;
 auto truncated=v4; truncated.pop_back(); before=truncated; if(rewrite_address(truncated,target,true)||truncated!=before) return 6;
}

