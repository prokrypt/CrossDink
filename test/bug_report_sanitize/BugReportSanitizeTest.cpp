#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "BugReportSanitize.h"

namespace {
using BugReportSanitize::Secret;

const Secret SECRETS[] = {{"HomeNet-5G", "[wifi1]"},
                          {"hunter2pw", "[pw]"},
                          {"opdsuser", "[opds-user]"},
                          {"482915", "[token]"},
                          {"ab", "[short]"}};

std::string clean(const std::string& line) {
  char out[512];
  const size_t n = BugReportSanitize::sanitizeLine(line.data(), line.size(), out, sizeof(out), SECRETS,
                                                   sizeof(SECRETS) / sizeof(SECRETS[0]));
  EXPECT_EQ(n, std::strlen(out));
  return out;
}

TEST(BugReportSanitize, KnownSecretsBecomeTags) {
  EXPECT_EQ(clean("[WIFI] joining HomeNet-5G with hunter2pw"), "[WIFI] joining [wifi1] with [pw]");
  EXPECT_EQ(clean("remote pin 482915 ok"), "remote pin [token] ok");
  // Too short to replace safely.
  EXPECT_EQ(clean("table ab"), "table ab");
}

TEST(BugReportSanitize, UrlUserinfo) {
  EXPECT_EQ(clean("GET http://opdsuser:s3cr3t@lib.example.com/feed"), "GET http://[auth]@lib.example.com/feed");
  EXPECT_EQ(clean("GET https://lib.example.com/feed"), "GET https://lib.example.com/feed");
}

TEST(BugReportSanitize, SecretKeysAndJson) {
  EXPECT_EQ(clean("/api/x?token=abc123&page=2"), "/api/x?token=[x]&page=2");
  EXPECT_EQ(clean("login password=letmein user=bob"), "login password=[x] user=bob");
  EXPECT_EQ(clean("{\"userkey\":\"0123abcd\",\"n\":1}"), "{\"userkey\":\"[x]\",\"n\":1}");
  // Lookalike names stay.
  EXPECT_EQ(clean("spine=3 keys=2 pages=10"), "spine=3 keys=2 pages=10");
}

TEST(BugReportSanitize, Headers) {
  EXPECT_EQ(clean("Authorization: Basic Ym9iOnB3"), "Authorization: [x]");
  EXPECT_EQ(clean("hdr x-auth-key: deadbeef"), "hdr x-auth-key: [x]");
  EXPECT_EQ(clean("X-Token: 999999"), "X-Token: [x]");
}

TEST(BugReportSanitize, MacAndIp) {
  EXPECT_EQ(clean("bssid AA:bb:01:23:45:67 ch 6"), "bssid [mac] ch 6");
  EXPECT_EQ(clean("got ip 10.0.1.67 gw 192.168.1.1 dns 8.8.8.8."), "got ip 10.0.1.67 gw 192.168.1.1 dns [ip].");
  EXPECT_EQ(clean("peer 172.20.0.5 vs 172.32.0.5"), "peer 172.20.0.5 vs [ip]");
  EXPECT_EQ(clean("ver 1.2.3.4.5 time 12:30:45"), "ver 1.2.3.4.5 time 12:30:45");
}

TEST(BugReportSanitize, NoSecretSurvivesMixedLine) {
  const std::string out =
      clean("[OPDS] http://opdsuser:hunter2pw@93.184.216.34/x?apikey=Q1w2 Authorization: Bearer zz");
  for (const char* secret : {"opdsuser", "hunter2pw", "93.184.216.34", "Q1w2", "Bearer zz"}) {
    EXPECT_EQ(out.find(secret), std::string::npos) << secret << " in " << out;
  }
}

TEST(BugReportSanitize, CutsAtCapacity) {
  char out[8];
  const std::string line = "0123456789abcdef";
  const size_t n = BugReportSanitize::sanitizeLine(line.data(), line.size(), out, sizeof(out), nullptr, 0);
  EXPECT_EQ(n, 7u);
  EXPECT_STREQ(out, "0123456");
}
}  // namespace
