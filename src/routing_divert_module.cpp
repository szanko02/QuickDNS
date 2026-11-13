#include "routing_divert_module.h"
namespace quickdns {
DivertModule::~DivertModule() = default;
bool DivertModule::load(const std::filesystem::path& path) {
    if(module_ || !path.is_absolute()) return false;
    HMODULE next=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!next) return false;
    DivertApi api;
    api.open=reinterpret_cast<decltype(api.open)>(GetProcAddress(next,"WinDivertOpen"));
    api.recv=reinterpret_cast<decltype(api.recv)>(GetProcAddress(next,"WinDivertRecv"));
    api.shutdown=reinterpret_cast<decltype(api.shutdown)>(GetProcAddress(next,"WinDivertShutdown"));
    api.close=reinterpret_cast<decltype(api.close)>(GetProcAddress(next,"WinDivertClose"));
    api.send=reinterpret_cast<decltype(api.send)>(GetProcAddress(next,"WinDivertSend"));
    if (!api.open || !api.recv || !api.shutdown || !api.close || !api.send) { FreeLibrary(next); return false; }
    api.library=std::shared_ptr<void>(next,[](void* handle){ FreeLibrary(static_cast<HMODULE>(handle)); });
    module_=next; api_=std::move(api); return true;
}
}


