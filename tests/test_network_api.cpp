#include <gtest/gtest.h>
#include "NetworkAPI.h"
#include <atomic>

TEST(NetworkAPITest, ParseBalanceJsonEmpty) {
    auto res = NetworkAPI::ParseBalanceJson("");
    EXPECT_TRUE(res.empty());
}

TEST(NetworkAPITest, ParseBalanceJsonSingleAddressFresh) {
    const std::string json = R"({
      "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU": {
        "final_balance": 0,
        "n_tx": 0,
        "total_received": 0
      }
    })";

    auto res = NetworkAPI::ParseBalanceJson(json);
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res["1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"].balance_satoshis, 0ull);
    EXPECT_EQ(res["1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"].tx_count, 0u);
}

TEST(NetworkAPITest, ParseBalanceJsonUsedAndFundedAddresses) {
    const std::string json = R"({
      "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU": {
        "final_balance": 0,
        "n_tx": 42,
        "total_received": 5000000
      },
      "1JTK7s9YVYywfm5XUH7RNhHJH1LshCaRFR": {
        "final_balance": 720014379,
        "n_tx": 1,
        "total_received": 720014379
      }
    })";

    auto res = NetworkAPI::ParseBalanceJson(json);
    ASSERT_EQ(res.size(), 2u);
    // Used but empty:
    EXPECT_EQ(res["1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"].balance_satoshis, 0ull);
    EXPECT_EQ(res["1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"].tx_count, 42u);
    // Funded:
    EXPECT_EQ(res["1JTK7s9YVYywfm5XUH7RNhHJH1LshCaRFR"].balance_satoshis, 720014379ull);
    EXPECT_EQ(res["1JTK7s9YVYywfm5XUH7RNhHJH1LshCaRFR"].tx_count, 1u);
}

TEST(NetworkAPITest, ParseBalanceJsonMalformed) {
    EXPECT_THROW(NetworkAPI::ParseBalanceJson("not valid json"), std::runtime_error);
    EXPECT_THROW(NetworkAPI::ParseBalanceJson("[\"array\", \"not\", \"object\"]"), std::runtime_error);
}

TEST(NetworkAPITest, QueryBalancesEmptyListReturnsEmpty) {
    auto res = NetworkAPI::QueryBalances({});
    EXPECT_TRUE(res.empty());
}

TEST(NetworkAPITest, CachingAndMockFetcher) {
    NetworkAPI::ClearCache();
    EXPECT_EQ(NetworkAPI::CacheSize(), 0u);

    std::atomic<int> fetch_call_count{0};
    NetworkAPI::SetHttpFetcherForTesting([&fetch_call_count](const std::string&, int) -> std::string {
        fetch_call_count++;
        return R"({
          "1AddrTest1": {
            "final_balance": 1000,
            "n_tx": 2
          }
        })";
    });

    // First query: cache miss, triggers mock fetcher
    auto res1 = NetworkAPI::QueryBalances({"1AddrTest1"});
    EXPECT_EQ(fetch_call_count.load(), 1);
    ASSERT_EQ(res1.size(), 1u);
    EXPECT_EQ(res1["1AddrTest1"].balance_satoshis, 1000ull);
    EXPECT_EQ(res1["1AddrTest1"].tx_count, 2u);
    EXPECT_TRUE(NetworkAPI::IsCached("1AddrTest1"));

    // Second query: cache hit, mock fetcher NOT called
    auto res2 = NetworkAPI::QueryBalances({"1AddrTest1"});
    EXPECT_EQ(fetch_call_count.load(), 1); // still 1
    ASSERT_EQ(res2.size(), 1u);
    EXPECT_EQ(res2["1AddrTest1"].balance_satoshis, 1000ull);
    EXPECT_EQ(res2["1AddrTest1"].tx_count, 2u);

    // Query combination of cached and new address
    NetworkAPI::SetHttpFetcherForTesting([&fetch_call_count](const std::string& url, int) -> std::string {
        fetch_call_count++;
        // Verify only missing address is requested in URL
        EXPECT_NE(url.find("1AddrTest2"), std::string::npos);
        EXPECT_EQ(url.find("1AddrTest1"), std::string::npos);
        return R"({
          "1AddrTest2": {
            "final_balance": 0,
            "n_tx": 0
          }
        })";
    });

    auto res3 = NetworkAPI::QueryBalances({"1AddrTest1", "1AddrTest2"});
    EXPECT_EQ(fetch_call_count.load(), 2);
    ASSERT_EQ(res3.size(), 2u);
    EXPECT_EQ(res3["1AddrTest1"].balance_satoshis, 1000ull);
    EXPECT_EQ(res3["1AddrTest2"].balance_satoshis, 0ull);
    EXPECT_EQ(res3["1AddrTest2"].tx_count, 0u);

    // Clean up
    NetworkAPI::ResetHttpFetcher();
    NetworkAPI::ClearCache();
    EXPECT_EQ(NetworkAPI::CacheSize(), 0u);
}
