// TargetListLoader tests: address files, prefix files (hex + Base58),
// comment/blank handling and exact line numbers in error messages.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "TargetListLoader.h"
#include "test_support.h"

using namespace puzzle;
using puzzle::test::TempDir;
using puzzle::test::WriteRaw;

namespace {

const char* const kPuzzle71 = "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU";
const char* const kGenesis = "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa";
const char* const kOther = "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2";

std::string RuntimeErrorMessage(const std::filesystem::path& p, bool addresses) {
  try {
    if (addresses) {
      (void)TargetListLoader::loadAddresses(p);
    } else {
      (void)TargetListLoader::loadPrefixes(p);
    }
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return "";
}

}  // namespace

TEST(TargetListLoaderAddresses, EmptyFileYieldsNoAddresses) {
  TempDir tmp;
  WriteRaw(tmp.path() / "a.txt", "");
  EXPECT_TRUE(TargetListLoader::loadAddresses(tmp.path() / "a.txt").empty());
}

TEST(TargetListLoaderAddresses, ValidFileLoadsInOrder) {
  TempDir tmp;
  WriteRaw(tmp.path() / "a.txt",
           std::string(kPuzzle71) + "\n" + kGenesis + "\n" + kOther + "\n");
  auto v = TargetListLoader::loadAddresses(tmp.path() / "a.txt");
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0], kPuzzle71);
  EXPECT_EQ(v[1], kGenesis);
  EXPECT_EQ(v[2], kOther);
}

TEST(TargetListLoaderAddresses, CommentsBlankLinesAndCrlfAreIgnored) {
  TempDir tmp;
  WriteRaw(tmp.path() / "a.txt",
           std::string("# header\r\n\r\n  ") + kPuzzle71 + "  \r\n   \n# x\n" +
               kGenesis);
  auto v = TargetListLoader::loadAddresses(tmp.path() / "a.txt");
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], kPuzzle71);
  EXPECT_EQ(v[1], kGenesis);
}

TEST(TargetListLoaderAddresses, BadChecksumReportsExactLineNumber) {
  TempDir tmp;
  WriteRaw(tmp.path() / "a.txt",
           std::string("# c\n") + kPuzzle71 + "\n\n" +
               "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXV\n");
  std::string msg = RuntimeErrorMessage(tmp.path() / "a.txt", true);
  EXPECT_NE(msg.find("line 4"), std::string::npos) << msg;
}

TEST(TargetListLoaderAddresses, InvalidCharactersThrowRuntimeErrorWithLine) {
  TempDir tmp;
  WriteRaw(tmp.path() / "a.txt", std::string(kPuzzle71) + "\n1PWo0OIl\n");
  EXPECT_THROW(TargetListLoader::loadAddresses(tmp.path() / "a.txt"),
               std::runtime_error);
  EXPECT_NE(RuntimeErrorMessage(tmp.path() / "a.txt", true).find("line 2"),
            std::string::npos);
}

TEST(TargetListLoaderAddresses, MissingFileThrows) {
  TempDir tmp;
  EXPECT_THROW(TargetListLoader::loadAddresses(tmp.path() / "nope.txt"),
               std::runtime_error);
}

TEST(TargetListLoaderPrefixes, EmptyFileYieldsNothing) {
  TempDir tmp;
  WriteRaw(tmp.path() / "p.txt", "\n# only comments\n\n");
  auto r = TargetListLoader::loadPrefixes(tmp.path() / "p.txt");
  EXPECT_TRUE(r.hex_prefixes.empty());
  EXPECT_TRUE(r.base58_prefixes.empty());
}

TEST(TargetListLoaderPrefixes, SplitsHexAndBase58) {
  TempDir tmp;
  WriteRaw(tmp.path() / "p.txt",
           "# vanity targets\n5ABC\n1PWo\n  6123  \n1PWo3J\n");
  auto r = TargetListLoader::loadPrefixes(tmp.path() / "p.txt");
  EXPECT_EQ(r.hex_prefixes, (std::vector<std::string>{"5abc", "6123"}));
  EXPECT_EQ(r.base58_prefixes, (std::vector<std::string>{"1PWo", "1PWo3J"}));
}

TEST(TargetListLoaderPrefixes, InvalidEntryReportsExactLine) {
  TempDir tmp;
  WriteRaw(tmp.path() / "p.txt", "5abc\n# c\n1PWo\n0x5a\n");
  // "0x5a": '0' and 'x' ... 'x' is valid Base58, '0' is not -> neither hex (0x
  // prefix rejected) nor Base58.
  std::string msg = RuntimeErrorMessage(tmp.path() / "p.txt", false);
  EXPECT_NE(msg.find("line 4"), std::string::npos) << msg;
}

TEST(TargetListLoaderPrefixes, NonAlphabetCharactersRejected) {
  TempDir tmp;
  WriteRaw(tmp.path() / "p.txt", "5abc\nzz!!\n");
  std::string msg = RuntimeErrorMessage(tmp.path() / "p.txt", false);
  EXPECT_NE(msg.find("line 2"), std::string::npos) << msg;
}

TEST(TargetListLoaderPrefixes, StrictHexAndBase58Loaders) {
  TempDir tmp;
  WriteRaw(tmp.path() / "h.txt", "5abc\nFF\n");
  EXPECT_EQ(TargetListLoader::loadHexPrefixes(tmp.path() / "h.txt"),
            (std::vector<std::string>{"5abc", "ff"}));
  WriteRaw(tmp.path() / "h2.txt", "5abc\nxyz\n");
  EXPECT_THROW(TargetListLoader::loadHexPrefixes(tmp.path() / "h2.txt"),
               std::runtime_error);
  WriteRaw(tmp.path() / "b.txt", "1PWo\n1A1z\n");
  EXPECT_EQ(TargetListLoader::loadBase58Prefixes(tmp.path() / "b.txt").size(), 2u);
  WriteRaw(tmp.path() / "b2.txt", "1PWo\n1l0O\n");
  EXPECT_THROW(TargetListLoader::loadBase58Prefixes(tmp.path() / "b2.txt"),
               std::runtime_error);
}

