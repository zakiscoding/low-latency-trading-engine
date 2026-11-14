#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lle/safety.hpp"

using namespace lle::safety;

TEST(Safety, PaperEndpointIsTheOnlyOrderHost) {
  EXPECT_EQ(check_endpoint("https://paper-api.alpaca.markets/v2/orders"), Verdict::Allowed);
  // Live Alpaca trading host, built at runtime so the repo scan stays meaningful.
  const std::string live = std::string("https://") + "api." + "alpaca.markets/v2/orders";
  EXPECT_EQ(check_endpoint(live), Verdict::Refused);
  EXPECT_EQ(check_endpoint(std::string("https://broker-api.") + "alpaca.markets"), Verdict::Refused);
  const std::string cb_orders = std::string("https://api.") + "coinbase.com/api/v3/brokerage/orders";
  EXPECT_EQ(check_endpoint(cb_orders), Verdict::Refused);
  EXPECT_EQ(check_endpoint("https://example.com"), Verdict::Refused);
}

TEST(Safety, MarketDataAndLocalHostsAllowed) {
  EXPECT_EQ(check_endpoint("wss://advanced-trade-ws.coinbase.com"), Verdict::Allowed);
  EXPECT_EQ(check_endpoint("wss://stream.data.alpaca.markets/v2/iex"), Verdict::Allowed);
  EXPECT_EQ(check_endpoint("udp://127.0.0.1:31002"), Verdict::Allowed);
  EXPECT_EQ(check_endpoint("udp://192.168.1.5:31002"), Verdict::Allowed);
  EXPECT_EQ(check_endpoint("udp://172.32.0.1:1"), Verdict::Refused);  // not RFC 1918
  EXPECT_EQ(check_endpoint("udp://8.8.8.8:53"), Verdict::Refused);
}

TEST(Safety, EnvironmentScan) {
  auto scan = [](std::vector<std::string> kv) {
    std::vector<char*> p;
    for (auto& s : kv) p.push_back(s.data());
    p.push_back(nullptr);
    return scan_environment(p.data());
  };
  EXPECT_EQ(scan({"PATH=/bin", "APCA_API_KEY_ID=PKTEST123"}), "");
  EXPECT_NE(scan({"APCA_API_KEY_ID=AKLIVE123"}), "");
  EXPECT_EQ(scan({"APCA_API_BASE_URL=https://paper-api.alpaca.markets"}), "");
  EXPECT_NE(scan({std::string("APCA_API_BASE_URL=https://api.") + "alpaca.markets"}), "");
  EXPECT_NE(scan({"COINBASE_API_KEY=x"}), "");
  EXPECT_EQ(scan({"COINBASE_API_KEY="}), "");  // empty is fine
}

TEST(Safety, RefusesToStartWithLiveKey) {
  std::string k = "APCA_API_KEY_ID=AKLIVE";
  char* env[] = {k.data(), nullptr};
  EXPECT_EXIT(enforce_paper_only(env), ::testing::ExitedWithCode(78), "REFUSING TO START");
}
