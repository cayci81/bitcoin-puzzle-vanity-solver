#include <gtest/gtest.h>
#include "NetworkAPI.h"

TEST(NetworkAPITest, ParseBalanceJsonEmpty) {
    auto res = NetworkAPI::ParseBalanceJson("");
    EXPECT_TRUE(res.empty());
}

TEST(NetworkAPITest, ParseBalanceJsonSingleAddress) {
    const std::string json = R"({
      "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU": {
        "final_balance": 0,
        "n_tx": 0,
        "total_received": 0
      }
    })";

    auto res = NetworkAPI::ParseBalanceJson(json);
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res["1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"], 0ull);
}

TEST(NetworkAPITest, ParseBalanceJsonMultipleAddresses) {
    const std::string json = R"({
      "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU": {
        "final_balance": 0,
        "n_tx": 0,
        "total_received": 0
      },
      "1JTK7s9YVYywfm5XUH7RNhHJH1LshCaRFR": {
        "final_balance": 720014379,
        "n_tx": 1,
        "total_received": 720014379
      }
    })";

    auto res = NetworkAPI::ParseBalanceJson(json);
    ASSERT_EQ(res.size(), 2u);
    EXPECT_EQ(res["1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"], 0ull);
    EXPECT_EQ(res["1JTK7s9YVYywfm5XUH7RNhHJH1LshCaRFR"], 720014379ull);
}

TEST(NetworkAPITest, ParseBalanceJsonMalformed) {
    EXPECT_THROW(NetworkAPI::ParseBalanceJson("not valid json"), std::runtime_error);
    EXPECT_THROW(NetworkAPI::ParseBalanceJson("[\"array\", \"not\", \"object\"]"), std::runtime_error);
}

TEST(NetworkAPITest, QueryBalancesEmptyListReturnsEmpty) {
    // Calling with empty addresses list returns empty map immediately with zero network overhead
    auto res = NetworkAPI::QueryBalances({});
    EXPECT_TRUE(res.empty());
}

