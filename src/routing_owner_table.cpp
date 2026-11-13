#include "routing_owner_table.h"
#include <algorithm>
#include <cstring>
#include <iphlpapi.h>
namespace quickdns {
namespace {
bool same_address(const std::array<uint8_t,16>& a, const std::array<uint8_t,16>& b, uint8_t family) {
    return std::equal(a.begin(), a.begin() + (family == 4 ? 4 : 16), b.begin());
}
bool link_local(const std::array<uint8_t,16>& a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }
bool mapped_ipv4(const std::array<uint8_t,16>& address, const std::array<uint8_t,16>& ipv4) {
    return std::all_of(address.begin(), address.begin()+10, [](auto b){ return b==0; }) &&
        address[10]==0xff && address[11]==0xff && std::equal(address.begin()+12,address.end(),ipv4.begin());
}
bool matches(const FlowKey& key, const OwnerObservation& row) {
    if (row.flow.protocol != key.protocol || row.flow.local_port != key.local_port) return false;
    // An IPv6 wildcard or IPv4-mapped socket may also own an IPv4 packet.
    if (key.family == 4 && row.flow.family == 6) {
        if (!row.wildcard_local && !mapped_ipv4(row.flow.local,key.local)) return false;
        return key.protocol == 17 || (row.flow.remote_port == key.remote_port && mapped_ipv4(row.flow.remote,key.remote));
    }
    if (row.flow.family != key.family) return false;
    if (!row.wildcard_local && !same_address(row.flow.local, key.local, key.family)) return false;
    return key.protocol == 17 || (row.flow.remote_port == key.remote_port && same_address(row.flow.remote, key.remote, key.family));
}
const OwnerObservation* unique_match(const FlowKey& key, const std::vector<OwnerObservation>& rows) {
    const OwnerObservation* match = nullptr;
    for (const auto& row : rows) {
        if (!matches(key, row)) continue;
        if (match || row.scope_ambiguous || (key.family != row.flow.family)) return nullptr;
        match = &row;
    }
    return match;
}
}
std::optional<OwnerObservation> resolve_owner_tables(const FlowKey& key, const std::vector<OwnerObservation>& first, const std::vector<OwnerObservation>& second) {
    if ((key.family != 4 && key.family != 6) || (key.protocol != 6 && key.protocol != 17) ||
        !key.local_port || (key.family == 6 && (link_local(key.local) || link_local(key.remote)))) return std::nullopt;
    const auto* a = unique_match(key, first);
    const auto* b = unique_match(key, second);
    if (!a || !b || !a->pid || !a->creation_time || a->owner == PacketOwner::Unknown ||
        a->pid != b->pid || a->creation_time != b->creation_time || a->owner != b->owner ||
        a->flow != b->flow || a->wildcard_local != b->wildcard_local) return std::nullopt;
    return *a;
}
PacketOwner classify_owner_tables(const FlowKey& key, const std::vector<OwnerObservation>& first, const std::vector<OwnerObservation>& second) {
    const auto resolved = resolve_owner_tables(key,first,second);
    return resolved ? resolved->owner : PacketOwner::Unknown;
}
namespace {
bool read_table(bool tcp, int family, std::vector<unsigned char>& buffer) {
    ULONG bytes = 0;
    DWORD status = tcp ? GetExtendedTcpTable(nullptr, &bytes, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0)
                       : GetExtendedUdpTable(nullptr, &bytes, FALSE, family, UDP_TABLE_OWNER_PID, 0);
    if (status != ERROR_INSUFFICIENT_BUFFER || !bytes) return false;
    for (int i = 0; i < 4; ++i) {
        buffer.resize(bytes);
        status = tcp ? GetExtendedTcpTable(buffer.data(), &bytes, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0)
                     : GetExtendedUdpTable(buffer.data(), &bytes, FALSE, family, UDP_TABLE_OWNER_PID, 0);
        if (status == NO_ERROR) return true;
        if (status != ERROR_INSUFFICIENT_BUFFER || !bytes) return false;
    }
    return false;
}
bool zero_address(const std::array<uint8_t,16>& address, std::size_t length) {
    return std::all_of(address.begin(), address.begin()+length, [](auto b){return b==0;});
}
void observe_process(OwnerObservation& row, ProcessOwnerClassifier classify, void* context) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, row.pid);
    if (!process) return;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        row.creation_time = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        row.owner = classify(process, context);
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) { row.owner = PacketOwner::Unknown; row.creation_time = 0; }
    }
    CloseHandle(process);
}
}
bool capture_owner_table_snapshot(const FlowKey& packet, ProcessOwnerClassifier classifier, void* context, std::vector<OwnerObservation>& result) {
    result.clear();
    if (!classifier || (packet.family != 4 && packet.family != 6) || (packet.protocol != 6 && packet.protocol != 17)) return false;
    std::vector<unsigned char> tcp4, tcp6, udp4, udp6;
    if (!read_table(true, AF_INET, tcp4) || !read_table(true, AF_INET6, tcp6) ||
        !read_table(false, AF_INET, udp4) || !read_table(false, AF_INET6, udp6)) return false;
    std::vector<OwnerObservation> found;
    const auto add = [&](OwnerObservation row) {
        if (!matches(packet, row)) return;
        observe_process(row, classifier, context);
        found.push_back(row);
    };
    if (packet.protocol == 6) {
        const auto* t4 = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(tcp4.data());
        for (DWORD i=0; i<t4->dwNumEntries; ++i) {
            const auto& r=t4->table[i]; OwnerObservation o{}; o.flow.family=4; o.flow.protocol=6;
            std::memcpy(o.flow.local.data(), &r.dwLocalAddr, 4); std::memcpy(o.flow.remote.data(), &r.dwRemoteAddr, 4);
            o.flow.local_port=ntohs(static_cast<u_short>(r.dwLocalPort)); o.flow.remote_port=ntohs(static_cast<u_short>(r.dwRemotePort)); o.pid=r.dwOwningPid;
            add(o);
        }
        const auto* t6 = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(tcp6.data());
        for (DWORD i=0; i<t6->dwNumEntries; ++i) {
            const auto& r=t6->table[i]; OwnerObservation o{}; o.flow.family=6; o.flow.protocol=6;
            std::memcpy(o.flow.local.data(), r.ucLocalAddr, 16); std::memcpy(o.flow.remote.data(), r.ucRemoteAddr, 16);
            o.flow.local_port=ntohs(static_cast<u_short>(r.dwLocalPort)); o.flow.remote_port=ntohs(static_cast<u_short>(r.dwRemotePort)); o.pid=r.dwOwningPid;
            o.scope_ambiguous=r.dwLocalScopeId != 0 || r.dwRemoteScopeId != 0; add(o);
        }
    } else {
        const auto* t4 = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(udp4.data());
        for (DWORD i=0; i<t4->dwNumEntries; ++i) {
            const auto& r=t4->table[i]; OwnerObservation o{}; o.flow.family=4; o.flow.protocol=17;
            std::memcpy(o.flow.local.data(), &r.dwLocalAddr, 4); o.flow.local_port=ntohs(static_cast<u_short>(r.dwLocalPort));
            o.pid=r.dwOwningPid; o.wildcard_local=zero_address(o.flow.local,4); add(o);
        }
        const auto* t6 = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(udp6.data());
        for (DWORD i=0; i<t6->dwNumEntries; ++i) {
            const auto& r=t6->table[i]; OwnerObservation o{}; o.flow.family=6; o.flow.protocol=17;
            std::memcpy(o.flow.local.data(), r.ucLocalAddr, 16); o.flow.local_port=ntohs(static_cast<u_short>(r.dwLocalPort));
            o.pid=r.dwOwningPid; o.wildcard_local=zero_address(o.flow.local,16); o.scope_ambiguous=r.dwLocalScopeId != 0; add(o);
        }
    }
    result = std::move(found);
    return true;
}
}
