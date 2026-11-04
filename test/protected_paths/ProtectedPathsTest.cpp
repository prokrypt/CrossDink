#include <gtest/gtest.h>

#include "network/ProtectedPaths.h"

using protectedpaths::isPluginPath;
using protectedpaths::isSensitivePath;

TEST(ProtectedPaths, CredentialStoresAreSensitive) {
  EXPECT_TRUE(isSensitivePath("/.crossdink/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crossdink/opds.json"));
  EXPECT_TRUE(isSensitivePath("/.crossdink/koreader.json"));
  EXPECT_TRUE(isSensitivePath("/.crossdink/wifi.json.tmp"));
  EXPECT_TRUE(isSensitivePath("/.CrossPoint/WIFI.json"));
}

TEST(ProtectedPaths, SpellingVariantsCannotDodgeTheMatch) {
  EXPECT_TRUE(isSensitivePath("/.crossdink/./wifi.json"));
  EXPECT_TRUE(isSensitivePath("//.crossdink//wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crossdink/plugins/../wifi.json"));
  EXPECT_TRUE(isSensitivePath(".crosspoint/opds.json"));
}

TEST(ProtectedPaths, NamesSdFatWouldTrimOrAliasStillMatch) {
  EXPECT_TRUE(isSensitivePath("/.crossdink/wifi.json."));
  EXPECT_TRUE(isSensitivePath("/.crossdink./wifi.json"));
  EXPECT_TRUE(isSensitivePath("/ .crosspoint/ wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crossdink ./opds.json"));
  EXPECT_TRUE(isSensitivePath("/CROSSD~1/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crossdink/WIFI~1.JSO"));
  EXPECT_FALSE(isSensitivePath("/Books/Photos/IMG~1.JPG"));  // too deep to reach a store
  EXPECT_FALSE(isSensitivePath("/Books/a~b.epub"));
}

TEST(ProtectedPaths, EverythingElseStaysReachable) {
  EXPECT_FALSE(isSensitivePath("/.crossdink/settings.json"));
  EXPECT_FALSE(isSensitivePath("/.crossdink/library.idx"));
  EXPECT_FALSE(isSensitivePath("/.crossdink/month-wallpaper.json"));
  EXPECT_FALSE(isSensitivePath("/.fonts/Noto/Noto_14.cpfont"));
  EXPECT_FALSE(isSensitivePath("/Books/wifi.json"));
}

TEST(ProtectedPaths, PluginPathRules) {
  EXPECT_FALSE(isPluginPath("/.crossdink/plugins/../wifi.json"));
  EXPECT_FALSE(isPluginPath("/.crossdink/./koreader.json"));
  EXPECT_FALSE(isPluginPath("relative/path"));
  EXPECT_FALSE(isPluginPath("/"));
  EXPECT_TRUE(isPluginPath("/.crossdink/month-wallpaper.json"));
  EXPECT_TRUE(isPluginPath("/Books/new.epub"));
}

TEST(ProtectedPaths, LegacyStoresAndDirectoryMutationsStayProtected) {
  EXPECT_TRUE(isSensitivePath("/.crossdink/wifi.bin.bak"));
  EXPECT_TRUE(isSensitivePath("/.crossdink/plugin-install.txt.part"));
  EXPECT_FALSE(isSensitivePath("/.crossdink"));  // still browse other hidden files
  EXPECT_TRUE(isSensitivePath("/.crossdink", true));
  EXPECT_TRUE(isSensitivePath("/.crossdink/.", true));
  EXPECT_FALSE(isSensitivePath("/.crossdink/plugins", true));
  EXPECT_FALSE(isPluginPath(std::string("/books/new.epub\0/other", 22)));
  EXPECT_FALSE(isPluginPath("/books\\..\\.crosspoint\\wifi.json"));
}
