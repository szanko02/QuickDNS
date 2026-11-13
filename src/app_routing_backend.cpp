#include "app_routing_backend.h"
namespace quickdns {
AppRoutingBackend::AppRoutingBackend(GuardJournal& j, GuardWfpApi& g, PolicyTransactionApi& p, RoutingBackendPreflight& f)
    : journal_(j), guard_(g), policy_(p), preflight_(f) {}
GuardEnableResult AppRoutingBackend::enable(GuardJournalEntry, const GuardIdentity&, std::uint64_t) { return {ERROR_NOT_SUPPORTED}; }
DWORD AppRoutingBackend::disable(const std::string&) { return ERROR_NOT_SUPPORTED; }
}
