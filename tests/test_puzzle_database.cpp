#include <gtest/gtest.h>
#include "PuzzleDatabase.h"
#include <fstream>

TEST(PuzzleDatabaseTest, ParseJsonValid) {
    const std::string json_data = R"({
      "metadata": { "version": "1.0" },
      "puzzles": [
        {
          "id": 71,
          "start": "0x400000000000000000",
          "end": "0x7fffffffffffffffff",
          "address": "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
          "reward": 7.1,
          "bits": 271
        },
        {
          "id": 72,
          "start": "800000000000000000",
          "end": "ffffffffffffffffff",
          "address": "1JTK7s9YVYywfm5XUH7RNhHJH1LshCaRFR",
          "reward": 7.2,
          "bits": 272
        }
      ]
    })";

    auto puzzles = PuzzleDatabase::parseJson(json_data);
    ASSERT_EQ(puzzles.size(), 2u);

    EXPECT_EQ(puzzles[0].id, 71);
    EXPECT_EQ(puzzles[0].start_hex, "400000000000000000");
    EXPECT_EQ(puzzles[0].end_hex, "7fffffffffffffffff");
    EXPECT_EQ(puzzles[0].address, "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");
    EXPECT_DOUBLE_EQ(puzzles[0].reward, 7.1);
    EXPECT_EQ(puzzles[0].bits, 271);

    EXPECT_EQ(puzzles[1].id, 72);
    EXPECT_EQ(puzzles[1].start_hex, "800000000000000000");
    EXPECT_EQ(puzzles[1].end_hex, "ffffffffffffffffff");

    const auto* p71 = PuzzleDatabase::findById(puzzles, 71);
    ASSERT_NE(p71, nullptr);
    EXPECT_EQ(p71->bits, 271);

    const auto* p999 = PuzzleDatabase::findById(puzzles, 999);
    EXPECT_EQ(p999, nullptr);
}

TEST(PuzzleDatabaseTest, ParseJsonArrayDirectly) {
    const std::string json_data = R"([
      {
        "id": 5,
        "start": "0x10",
        "end": "0x1f",
        "address": "1Addr",
        "reward": 0.5,
        "bits": 5
      }
    ])";

    auto puzzles = PuzzleDatabase::parseJson(json_data);
    ASSERT_EQ(puzzles.size(), 1u);
    EXPECT_EQ(puzzles[0].id, 5);
    EXPECT_EQ(puzzles[0].start_hex, "10");
}

TEST(PuzzleDatabaseTest, ParseInvalidJsonThrows) {
    EXPECT_THROW(PuzzleDatabase::parseJson("not valid json"), std::runtime_error);
    EXPECT_THROW(PuzzleDatabase::parseJson("{\"metadata\": {}}"), std::runtime_error);
}

TEST(PuzzleDatabaseTest, LoadMissingFileThrowsWhenExplicit) {
    EXPECT_THROW(PuzzleDatabase::load("non_existent_puzzles_file_12345.json"), std::runtime_error);
}

TEST(PuzzleDatabaseTest, LoadDefaultCandidateOrEmpty) {
    // Should not crash even if file doesn't exist, returns empty or loaded puzzles
    EXPECT_NO_THROW({
        auto list = PuzzleDatabase::load("");
        (void)list;
    });
}

