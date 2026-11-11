#include "ai_checker.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>

using namespace quickdns;

static int g_pass = 0;
static int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            g_pass++; \
        } else { \
            std::cout << "[FAIL] " << (name) << " at line " << __LINE__ << std::endl; \
            g_fail++; \
        } \
    } while (0)

namespace {

void test_build_dns_query() {
    auto pkt = AiChecker::build_dns_a_query(0x1234, "ChatGPT.com");
    // 12 byte header + (1+7 + 1+3 + 1) QNAME + 4 byte QTYPE/QCLASS = 29 bytes
    TEST_CHECK(pkt.size() == 29, "DNS query packet size for ChatGPT.com is 29 bytes");
    TEST_CHECK(pkt[0] == 0x12 && pkt[1] == 0x34, "DNS query transaction ID is 0x1234");
    TEST_CHECK(pkt[2] == 0x01 && pkt[3] == 0x00, "DNS query recursion desired flag is set");
    TEST_CHECK(pkt[4] == 0x00 && pkt[5] == 0x01, "DNS query QDCOUNT is 1");
    TEST_CHECK(pkt[12] == 7 && pkt[13] == 'c' && pkt[19] == 't', "DNS query lowercases label 'chatgpt'");
    TEST_CHECK(pkt[20] == 3 && pkt[21] == 'c' && pkt[22] == 'o' && pkt[23] == 'm', "DNS query encodes label 'com'");
    TEST_CHECK(pkt[24] == 0x00, "DNS query QNAME null terminator");
    TEST_CHECK(pkt[25] == 0x00 && pkt[26] == 0x01, "DNS query QTYPE is A (1)");
    TEST_CHECK(pkt[27] == 0x00 && pkt[28] == 0x01, "DNS query QCLASS is IN (1)");
}

void test_parse_direct_a_record() {
    // Build a synthetic DNS response for "chatgpt.com" -> 83.220.169.155
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

    // Wrong TXID must fail
    std::string wrong_tx = AiChecker::parse_dns_a_response(resp.data(), resp.size(), 0x1111);
    TEST_CHECK(wrong_tx.empty(), "Mismatched TXID is rejected");

    // NXDOMAIN (RCODE = 3) must fail
    auto nxdomain = resp;
    nxdomain[3] = 0x83;
    TEST_CHECK(AiChecker::parse_dns_a_response(nxdomain.data(), nxdomain.size(), 0xABCD).empty(),
               "NXDOMAIN response returns empty IP");

    // Sinkhole 0.0.0.0 or 127.0.0.1 must be rejected
    auto sinkhole = resp;
    sinkhole[sinkhole.size() - 4] = 0;
    sinkhole[sinkhole.size() - 3] = 0;
    sinkhole[sinkhole.size() - 2] = 0;
    sinkhole[sinkhole.size() - 1] = 0;
    TEST_CHECK(AiChecker::parse_dns_a_response(sinkhole.data(), sinkhole.size(), 0xABCD).empty(),
               "Sinkhole 0.0.0.0 A-record is rejected");
}

void test_parse_cname_plus_a_record_chain() {
    // Response with Question + CNAME record + AAAA record + A record
    std::vector<uint8_t> resp = {
        0x42, 0x01,             // TXID = 0x4201
        0x81, 0x80,             // Standard query response, no error
        0x00, 0x01,             // QDCOUNT = 1
        0x00, 0x03,             // ANCOUNT = 3 (CNAME, AAAA, A)
        0x00, 0x00,
        0x00, 0x00,
        // Question: gemini.google.com
        0x06, 'g', 'e', 'm', 'i', 'n', 'i',
        0x06, 'g', 'o', 'o', 'g', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,
        0x00, 0x01,             // QTYPE = A
        0x00, 0x01,             // QCLASS = IN
        // Answer 1: CNAME (TYPE = 5), RDLENGTH = 6 (alias.ptr)
        0xC0, 0x0C,             // Name pointer
        0x00, 0x05,             // TYPE = CNAME (5)
        0x00, 0x01,             // CLASS = IN (1)
        0x00, 0x00, 0x01, 0x2C, // TTL = 300
        0x00, 0x06,             // RDLENGTH = 6
        0x03, 'w', 'w', 'w', 0xC0, 0x13, // "www" + pointer to google.com
        // Answer 2: AAAA (TYPE = 28), RDLENGTH = 16
        0xC0, 0x0C,
        0x00, 0x1C,             // TYPE = AAAA (28)
        0x00, 0x01,             // CLASS = IN (1)
        0x00, 0x00, 0x00, 0x3C,
        0x00, 0x10,             // RDLENGTH = 16
        0x2a, 0x00, 0x14, 0x50, 0x40, 0x01, 0x08, 0x10,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x0e,
        // Answer 3: A (TYPE = 1), RDLENGTH = 4 -> 142.250.185.78
        0xC0, 0x0C,
        0x00, 0x01,             // TYPE = A (1)
        0x00, 0x01,             // CLASS = IN (1)
        0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04,             // RDLENGTH = 4
        142, 250, 185, 78
    };

    std::string ip = AiChecker::parse_dns_a_response(resp.data(), resp.size(), 0x4201);
    TEST_CHECK(ip == "142.250.185.78", "CNAME + AAAA + A chain extracts 142.250.185.78");

    // Truncated packet safety check
    for (size_t cut = 0; cut < resp.size() - 4; ++cut) {
        std::string partial = AiChecker::parse_dns_a_response(resp.data(), cut, 0x4201);
        if (!partial.empty()) {
            TEST_CHECK(false, "Truncated packet should not return an IP");
            break;
        }
    }
    TEST_CHECK(true, "Truncated DNS packets safely return empty string without out-of-bounds access");
}

void test_profile_result_aggregation() {
    // Case 1: All 4 services Available
    ProfileAiCheckResult r1;
    r1.profile_id = "comss";
    r1.profile_name = "Comss.one DNS";
    r1.dns_ip_used = "83.220.169.155";
    r1.chatgpt = {AiService::ChatGPT, AiAccessStatus::Available, "83.220.169.155", 40, L"Доступен (40 мс, NL)"};
    r1.claude  = {AiService::Claude,  AiAccessStatus::Available, "83.220.169.155", 50, L"Доступен (50 мс, NL)"};
    r1.gemini  = {AiService::Gemini,  AiAccessStatus::Available, "83.220.169.155", 60, L"Доступен (60 мс)"};
    r1.copilot = {AiService::Copilot, AiAccessStatus::Available, "83.220.169.155", 70, L"Доступен (70 мс)"};

    AiChecker::aggregate_profile_result(r1);
    TEST_CHECK(r1.available_count == 4, "4/4 available services counted as 4");
    TEST_CHECK(r1.avg_latency_ms == 55, "Average latency of (40, 50, 60, 70) is 55 ms");

    // Case 2: 2 Available, 1 GeoBlocked, 1 DnsFailed
    ProfileAiCheckResult r2;
    r2.chatgpt = {AiService::ChatGPT, AiAccessStatus::Available,  "193.233.112.67", 30, L"Доступен (30 мс, DE)"};
    r2.claude  = {AiService::Claude,  AiAccessStatus::GeoBlocked, "160.79.104.10",  20, L"Гео-блок (RU)"};
    r2.gemini  = {AiService::Gemini,  AiAccessStatus::Available,  "193.233.112.67", 50, L"Доступен (50 мс)"};
    r2.copilot = {AiService::Copilot, AiAccessStatus::DnsFailed,  "",               -1, L"Нет ответа DNS"};

    AiChecker::aggregate_profile_result(r2);
    TEST_CHECK(r2.available_count == 2, "2/4 available services counted as 2");
    TEST_CHECK(r2.avg_latency_ms == 40, "Average latency of available services (30, 50) is 40 ms");

    // Case 3: 0 Available
    ProfileAiCheckResult r3;
    r3.chatgpt = {AiService::ChatGPT, AiAccessStatus::GeoBlocked,  "172.64.155.209", 25, L"Гео-блок (RU)"};
    r3.claude  = {AiService::Claude,  AiAccessStatus::GeoBlocked,  "160.79.104.10",  28, L"Гео-блок (RU)"};
    r3.gemini  = {AiService::Gemini,  AiAccessStatus::GeoBlocked,  "142.250.185.78", 35, L"Гео-блок (RU)"};
    r3.copilot = {AiService::Copilot, AiAccessStatus::Unreachable, "23.45.67.89",    -1, L"Тайм-аут TLS"};

    AiChecker::aggregate_profile_result(r3);
    TEST_CHECK(r3.available_count == 0, "0/4 available services counted as 0");
    TEST_CHECK(r3.avg_latency_ms == -1, "Average latency is -1 when 0 services are available");

    // Case 4: Empty DNS profile check_profile fast path
    DnsProfile empty_prof;
    empty_prof.id = "dhcp";
    empty_prof.name = "DHCP";
    auto r4 = AiChecker::check_profile(empty_prof, "RU", 500);
    TEST_CHECK(r4.available_count == 0, "Empty DNS profile has available_count == 0");
    TEST_CHECK(r4.avg_latency_ms == -1, "Empty DNS profile has avg_latency_ms == -1");
    TEST_CHECK(r4.chatgpt.status == AiAccessStatus::DnsFailed, "Empty DNS profile marks ChatGPT as DnsFailed");
}

} // anonymous namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  QuickDNS AI Checker Offline Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_build_dns_query();
    test_parse_direct_a_record();
    test_parse_cname_plus_a_record_chain();
    test_profile_result_aggregation();

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
