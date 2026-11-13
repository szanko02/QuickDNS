#include "routing_divert_module.h"
#include <filesystem>
int main(int argc, char** argv) {
 if (argc != 2) return 1;
 quickdns::DivertModule bad;
 if(bad.load(L"relative.dll")) return 2;
 if(bad.load(std::filesystem::absolute(L"missing.dll"))) return 3;
 const auto path=std::filesystem::absolute(argv[1]);
 quickdns::DivertApi saved;
 { quickdns::DivertModule module;
   if(!module.load(path)) return 4;
   saved=module.api(); if(!saved.open || !saved.recv || !saved.shutdown || !saved.close || !saved.send) return 5;
   if(module.load(path)) return 6;
 }
 if(!GetModuleHandleW(path.c_str())) return 7;
 saved={}; if(GetModuleHandleW(path.c_str())) return 8;
}
