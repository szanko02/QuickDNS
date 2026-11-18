#include "ai_checker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace quickdns;

namespace {

int g_pass = 0;
int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            ++g_pass; \
        } else { \
            std::cout << "[FAIL] " << (name) << " at line " << __LINE__ << std::endl; \
            ++g_fail; \
        } \
    } while (0)

void test_build_dns_query() {
    auto pkt = AiChecker::build_dns_a_query(0x1234, "ChatGPT.com");
    // 29 bytes without EDNS0 OPT (ARCOUNT=0) or 40 bytes with 11-byte EDNS0 OPT RR (ARCOUNT=1)
    bool valid_size = (pkt.size() == 29 && pkt[10] == 0x00 && pkt[11] == 0x00) ||
                      (pkt.size() == 40 && pkt[10] == 0x00 && pkt[11] == 0x01 && pkt[31] == 0x29);
    TEST_CHECK(valid_size, "DNS query packet size/ARCOUNT valid for ChatGPT.com");
    if (pkt.size() >= 29) {
        TEST_CHECK(pkt[0] == 0x12 && pkt[1] == 0x34, "DNS query transaction ID is 0x1234");
        TEST_CHECK((pkt[2] & 0x01) == 0x01 && (pkt[2] & 0x80) == 0x00, "DNS query QR=0 and RD=1 flags set");
        TEST_CHECK(pkt[4] == 0x00 && pkt[5] == 0x01, "DNS query QDCOUNT is 1");
        TEST_CHECK(pkt[12] == 7 && pkt[13] == 'c' && pkt[19] == 't', "DNS query lowercases label 'chatgpt'");
        TEST_CHECK(pkt[20] == 3 && pkt[21] == 'c' && pkt[22] == 'o' && pkt[23] == 'm', "DNS query encodes label 'com'");
        TEST_CHECK(pkt[24] == 0x00, "DNS query QNAME null terminator");
        TEST_CHECK(pkt[25] == 0x00 && pkt[26] == 0x01, "DNS query QTYPE is A (1)");
        TEST_CHECK(pkt[27] == 0x00 && pkt[28] == 0x01, "DNS query QCLASS is IN (1)");
    }

    // Trailing dot FQDN must succeed and match non-trailing wire QNAME
    auto trailing_pkt = AiChecker::build_dns_a_query(0x1234, "ChatGPT.com.");
    TEST_CHECK(trailing_pkt == pkt, "Trailing dot FQDN 'ChatGPT.com.' encodes identically");

    // Exact 63-octet label boundary must succeed
    std::string max_label(63, 'a');
    auto ok63_pkt = AiChecker::build_dns_a_query(0x2222, max_label + ".com");
    TEST_CHECK(!ok63_pkt.empty(), "63-octet DNS label is accepted per RFC 1035");

    // Malformed QNAME inputs must return empty vector
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, "").empty(), "Reject empty domain");
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, ".").empty(), "Reject single dot domain");
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, "..").empty(), "Reject double dot domain");
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, ".chatgpt.com").empty(), "Reject leading dot domain");
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, "chat..gpt.com").empty(), "Reject empty intermediate label '..'");

    std::string over_label(64, 'a');
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, over_label + ".com").empty(),
               "Reject >63-octet DNS label");

    std::string over_qname;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) over_qname += '.';
        over_qname += std::string(60, 'x');
    }
    TEST_CHECK(AiChecker::build_dns_a_query(0x2222, over_qname).empty(),
               "Reject total wire QNAME > 255 octets");
}

void test_parse_direct_a_record_and_bogon_filtering() {
    std::vector<uint8_t> resp = {
        0xAB, 0xCD,             // TXID = 0xABCD
        0x81, 0x80,             // Flags: QR=1, RD=1, RA=1, RCODE=0
        0x00, 0x01,             // QDCOUNT = 1
        0x00, 0x01,             // ANCOUNT = 1
        0x00, 0x00,             // NSCOUNT = 0
        0x00, 0x00,             // ARCOUNT = 0
        // Question: chatgpt.com
        0x07, 'c', 'h', 'a', 't', 'g', 'p', 't',
        0x03, 'c', 'o', 'm',
        0x00,
        0x00, 0x01,             // QTYPE = A
        0x00, 0x01,             // QCLASS = IN
        // Answer: pointer to 0x000C (chatgpt.com)
        0xC0, 0x0C,
        0x00, 0x01,             // TYPE = A (1)
        0x00, 0x01,             // CLASS = IN (1)
        0x00, 0x00, 0x00, 0x3C, // TTL = 60
        0x00, 0x04,             // RDLENGTH = 4
        83, 220, 169, 155       // RDATA = 83.220.169.155
    };

    std::string ip = AiChecker::parse_dns_a_response(resp.data(), resp.size(), 0xABCD);
    TEST_CHECK(ip == "83.220.169.155", "Direct A-record response parsed as 83.220.169.155");

    // Wrong TXID
    TEST_CHECK(AiChecker::parse_dns_a_response(resp.data(), resp.size(), 0x1111).empty(),
               "Mismatched TXID is rejected");

    // NXDOMAIN (RCODE=3), SERVFAIL (RCODE=2), REFUSED (RCODE=5)
    auto rcode_pkt = resp;
    rcode_pkt[3] = 0x83;
    TEST_CHECK(AiChecker::parse_dns_a_response(rcode_pkt.data(), rcode_pkt.size(), 0xABCD).empty(),
               "NXDOMAIN (RCODE=3) returns empty IP");
    rcode_pkt[3] = 0x82;
    TEST_CHECK(AiChecker::parse_dns_a_response(rcode_pkt.data(), rcode_pkt.size(), 0xABCD).empty(),
               "SERVFAIL (RCODE=2) returns empty IP");
    rcode_pkt[3] = 0x85;
    TEST_CHECK(AiChecker::parse_dns_a_response(rcode_pkt.data(), rcode_pkt.size(), 0xABCD).empty(),
               "REFUSED (RCODE=5) returns empty IP");

    // Sinkhole, loopback, RFC1918, CGNAT, link-local, multicast, reserved, broadcast A-records must all be rejected
    struct BogonCase {
        uint8_t a, b, c, d;
        const char* label;
    };
    const BogonCase bogons[] = {
        { 0, 0, 0, 0,       "Reject sinkhole 0.0.0.0" },
        { 127, 0, 0, 1,     "Reject loopback 127.0.0.1" },
        { 10, 0, 0, 1,      "Reject RFC1918 10.0.0.1" },
        { 100, 64, 0, 1,    "Reject CGNAT 100.64.0.1" },
        { 169, 254, 1, 1,   "Reject link-local 169.254.1.1" },
        { 172, 16, 0, 1,    "Reject RFC1918 172.16.0.1" },
        { 192, 168, 1, 1,   "Reject RFC1918 192.168.1.1" },
        { 224, 0, 0, 1,     "Reject multicast 224.0.0.1" },
        { 240, 0, 0, 1,     "Reject reserved 240.0.0.1" },
        { 255, 255, 255, 255, "Reject broadcast 255.255.255.255" }
    };

    for (const auto& bg : bogons) {
        auto pkt = resp;
        pkt[pkt.size() - 4] = bg.a;
        pkt[pkt.size() - 3] = bg.b;
        pkt[pkt.size() - 2] = bg.c;
        pkt[pkt.size() - 1] = bg.d;
        TEST_CHECK(AiChecker::parse_dns_a_response(pkt.data(), pkt.size(), 0xABCD).empty(), bg.label);
    }
}

void test_malformed_and_pointer_loop_packets() {
    // nullptr & empty buffer
    TEST_CHECK(AiChecker::parse_dns_a_response(nullptr, 64, 0xABCD).empty(),
               "Reject nullptr packet buffer safely");
    uint8_t dummy = 0;
    TEST_CHECK(AiChecker::parse_dns_a_response(&dummy, 0, 0xABCD).empty(),
               "Reject zero-length packet buffer safely");

    // Query packet (QR=0 instead of QR=1)
    std::vector<uint8_t> query_echo = {
        0xAB, 0xCD,
        0x01, 0x00, // QR=0
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00,
        0x07, 'c', 'h', 'a', 't', 'g', 'p', 't', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
        83, 220, 169, 155
    };
    TEST_CHECK(AiChecker::parse_dns_a_response(query_echo.data(), query_echo.size(), 0xABCD).empty(),
               "Reject DNS packet with QR=0 (query bit instead of response)");

    // Inflated QDCOUNT and ANCOUNT
    auto inflated_qd = query_echo;
    inflated_qd[2] = 0x81;
    inflated_qd[3] = 0x80;
    inflated_qd[5] = 0x10; // QDCOUNT = 16
    TEST_CHECK(AiChecker::parse_dns_a_response(inflated_qd.data(), inflated_qd.size(), 0xABCD).empty(),
               "Reject packet with inflated QDCOUNT safely");

    std::vector<uint8_t> inflated_an = {
        0xAB, 0xCD, 0x81, 0x80,
        0x00, 0x01,
        0x00, 0xFF, // ANCOUNT = 255 with 0 answer RRs in buffer
        0x00, 0x00, 0x00, 0x00,
        0x07, 'c', 'h', 'a', 't', 'g', 'p', 't', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01
    };
    TEST_CHECK(AiChecker::parse_dns_a_response(inflated_an.data(), inflated_an.size(), 0xABCD).empty(),
               "Reject packet with inflated ANCOUNT safely");

    // Self-referencing compression pointer 0xC0 0x0C at offset 0x0C
    std::vector<uint8_t> self_ptr_qname = {
        0xAB, 0xCD, 0x81, 0x80,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x0C, // offset 12 points back to offset 12!
        0x00, 0x01, 0x00, 0x01,
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
        83, 220, 169, 155
    };
    TEST_CHECK(AiChecker::parse_dns_a_response(self_ptr_qname.data(), self_ptr_qname.size(), 0xABCD).empty(),
               "Reject self-referencing compression pointer 0xC0 0x0C without infinite loop");

    // Self-referencing compression pointer in Answer RR owner name (offset 29 = 0x1D -> 0xC0 0x1D)
    std::vector<uint8_t> self_ptr_ans = {
        0xAB, 0xCD, 0x81, 0x80,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x07, 'c', 'h', 'a', 't', 'g', 'p', 't', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        0xC0, 0x1D, // offset 29 points to itself!
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
        83, 220, 169, 155
    };
    TEST_CHECK(AiChecker::parse_dns_a_response(self_ptr_ans.data(), self_ptr_ans.size(), 0xABCD).empty(),
               "Reject self-referencing Answer owner pointer 0xC0 0x1D");

    // Out-of-bounds compression pointer 0xC0 0xFF
    auto oob_ptr = self_ptr_ans;
    oob_ptr[29] = 0xC0;
    oob_ptr[30] = 0xFF;
    TEST_CHECK(AiChecker::parse_dns_a_response(oob_ptr.data(), oob_ptr.size(), 0xABCD).empty(),
               "Reject out-of-bounds compression pointer 0xC0 0xFF");
}

void test_parse_cname_plus_a_record_chain() {
    // Response with Question + CNAME record + AAAA record + flattened A record
    std::vector<uint8_t> resp = {
        0x42, 0x01,             // TXID = 0x4201
        0x81, 0x80,             // Standard query response, no error
        0x00, 0x01,             // QDCOUNT = 1
        0x00, 0x03,             // ANCOUNT = 3 (CNAME, AAAA, A)
        0x00, 0x00,
        0x00, 0x00,
        // Question (offset 12 / 0x0C): gemini.google.com ('google.com' starts at 19 / 0x13)
        0x06, 'g', 'e', 'm', 'i', 'n', 'i',
        0x06, 'g', 'o', 'o', 'g', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,
        0x00, 0x01,             // QTYPE = A
        0x00, 0x01,             // QCLASS = IN
        // Answer 1 (offset 35): CNAME (TYPE = 5), RDLENGTH = 6, RDATA starts at offset 47 (0x2F)
        0xC0, 0x0C,
        0x00, 0x05,             // TYPE = CNAME (5)
        0x00, 0x01,             // CLASS = IN (1)
        0x00, 0x00, 0x01, 0x2C,
        0x00, 0x06,
        0x03, 'w', 'w', 'w', 0xC0, 0x13, // "www.google.com" at offset 0x2F
        // Answer 2 (offset 53): AAAA (TYPE = 28), RDLENGTH = 16
        0xC0, 0x0C,
        0x00, 0x1C,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C,
        0x00, 0x10,
        0x2a, 0x00, 0x14, 0x50, 0x40, 0x01, 0x08, 0x10,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x0e,
        // Answer 3 (offset 81): A (TYPE = 1), RDLENGTH = 4 -> 142.250.185.78
        0xC0, 0x0C,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04,
        142, 250, 185, 78
    };

    std::string ip = AiChecker::parse_dns_a_response(resp.data(), resp.size(), 0x4201);
    TEST_CHECK(ip == "142.250.185.78", "Flattened CNAME + AAAA + A chain extracts 142.250.185.78");

    // Chained CNAME test: Answer 3 owner points to CNAME target (0xC0 0x2F -> www.google.com)
    auto chained = resp;
    chained[81] = 0xC0;
    chained[82] = 0x2F;
    std::string chained_ip = AiChecker::parse_dns_a_response(chained.data(), chained.size(), 0x4201);
    TEST_CHECK(chained_ip == "142.250.185.78", "Chained CNAME target owner (www.google.com) extracts 142.250.185.78");

    // Truncated packet safety check across ALL cut points (0 .. resp.size() - 1, including inside final 4B RDATA)
    bool truncated_ok = true;
    for (size_t cut = 0; cut < resp.size(); ++cut) {
        std::string partial = AiChecker::parse_dns_a_response(resp.data(), cut, 0x4201);
        if (!partial.empty()) {
            std::cout << "[DETAIL] Unexpected IP '" << partial << "' returned at truncated length " << cut << std::endl;
            truncated_ok = false;
            break;
        }
    }
    TEST_CHECK(truncated_ok, "All truncated DNS packet lengths (0 .. size-1) safely return empty string");
}

void test_profile_result_aggregation_and_cancellation() {
    // Case 1: All 4 services Available
    ProfileAiCheckResult r1;
    r1.profile_id = "comss";
    r1.profile_name = "Comss.one DNS";
    r1.dns_ip_used = "83.220.169.155";
    r1.chatgpt = {AiService::ChatGPT, AiAccessStatus::Available, "83.220.169.155", 40, L"Available"};
    r1.claude  = {AiService::Claude,  AiAccessStatus::Available, "83.220.169.155", 50, L"Available"};
    r1.gemini  = {AiService::Gemini,  AiAccessStatus::Available, "83.220.169.155", 60, L"Available"};
    r1.copilot = {AiService::Copilot, AiAccessStatus::Available, "83.220.169.155", 70, L"Available"};

    AiChecker::aggregate_profile_result(r1);
    TEST_CHECK(r1.available_count == 4, "4/4 available services counted as 4");
    TEST_CHECK(r1.avg_latency_ms == 55, "Average latency of (40, 50, 60, 70) is 55 ms");

    // Case 2: 2 Available, 1 GeoBlocked, 1 DnsFailed
    ProfileAiCheckResult r2;
    r2.chatgpt = {AiService::ChatGPT, AiAccessStatus::Available,  "193.233.112.67", 30, L"Available"};
    r2.claude  = {AiService::Claude,  AiAccessStatus::GeoBlocked, "160.79.104.10",  20, L"GeoBlocked"};
    r2.gemini  = {AiService::Gemini,  AiAccessStatus::Available,  "193.233.112.67", 50, L"Available"};
    r2.copilot = {AiService::Copilot, AiAccessStatus::DnsFailed,  "",               -1, L"DnsFailed"};

    AiChecker::aggregate_profile_result(r2);
    TEST_CHECK(r2.available_count == 2, "2/4 available services counted as 2");
    TEST_CHECK(r2.avg_latency_ms == 40, "Average latency of available services (30, 50) is 40 ms");

    // Case 3: 0 Available
    ProfileAiCheckResult r3;
    r3.chatgpt = {AiService::ChatGPT, AiAccessStatus::GeoBlocked,  "172.64.155.209", 25, L"GeoBlocked"};
    r3.claude  = {AiService::Claude,  AiAccessStatus::GeoBlocked,  "160.79.104.10",  28, L"GeoBlocked"};
    r3.gemini  = {AiService::Gemini,  AiAccessStatus::GeoBlocked,  "142.250.185.78", 35, L"GeoBlocked"};
    r3.copilot = {AiService::Copilot, AiAccessStatus::Unreachable, "23.45.67.89",    -1, L"Unreachable"};

    AiChecker::aggregate_profile_result(r3);
    TEST_CHECK(r3.available_count == 0, "0/4 available services counted as 0");
    TEST_CHECK(r3.avg_latency_ms == -1, "Average latency is -1 when 0 services are available");

    // Case 4: Empty DNS profile check_profile fast path
    DnsProfile empty_prof;
    empty_prof.id = "dhcp";
    empty_prof.name = "DHCP";
    auto r4 = AiChecker::check_profile(empty_prof, "RU", 500);
    TEST_CHECK(r4.available_count == 0 && r4.avg_latency_ms == -1 &&
               r4.chatgpt.status == AiAccessStatus::DnsFailed,
               "Empty DNS profile fast-fails with DnsFailed and 0 available");

    // Case 5: Pre-cancelled check_profiles exits immediately without network blocking or callbacks
    DnsProfile p1;
    p1.id = "comss";
    p1.name = "Comss";
    p1.primary_ipv4 = "83.220.169.155";

    DnsProfile p2;
    p2.id = "cloudflare";
    p2.name = "Cloudflare";
    p2.primary_ipv4 = "1.1.1.1";

    std::atomic<bool> cancel_flag{true};
    std::atomic<int> cb_calls{0};
    const auto t0 = std::chrono::steady_clock::now();
    auto cancelled_results = AiChecker::check_profiles(
        {p1, p2},
        "RU",
        &cancel_flag,
        [&](size_t, const ProfileAiCheckResult&) {
            ++cb_calls;
        }
    );
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0
    ).count();
    TEST_CHECK(cb_calls.load() == 0 && elapsed_ms < 500 && cancelled_results.size() <= 2,
               "Pre-cancelled check_profiles returns immediately with 0 callbacks");
}

} // anonymous namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  QuickDNS AI Checker Offline Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_build_dns_query();
    test_parse_direct_a_record_and_bogon_filtering();
    test_malformed_and_pointer_loop_packets();
    test_parse_cname_plus_a_record_chain();
    test_profile_result_aggregation_and_cancellation();

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
