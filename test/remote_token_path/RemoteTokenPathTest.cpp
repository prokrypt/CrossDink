#include <gtest/gtest.h>

#include "SerialRemote.h"

TEST(RemoteTokenPath, RefusesEverySpellingThatOpensTheToken) {
  for (const char* p : {"/debug/remote-token", "/DEBUG/Remote-Token", "debug/remote-token", "//debug//remote-token/",
                        "/debug/remote-token. ", "/debug./remote-token", "/debug/REMOTE~1", "/debug/./remote-token",
                        "/x/.../debug/remote-token"}) {
    EXPECT_TRUE(SerialRemote::isTokenPath(p)) << p;
  }
}

TEST(RemoteTokenPath, AllowsOtherDebugFiles) {
  for (const char* p : {"/", "", "/debug", "/debug/logs/battery.csv", "/debug/crash_report.txt", "/debug/remote-token2",
                        "/debugx/remote-token", "/a/debug/remote-token", "/debug/remote~"}) {
    EXPECT_FALSE(SerialRemote::isTokenPath(p)) << p;
  }
}

TEST(RemoteTokenPath, FolderOnlyWhenAsked) {
  EXPECT_FALSE(SerialRemote::isTokenPath("/debug"));
  EXPECT_TRUE(SerialRemote::isTokenPath("/Debug", true));
  EXPECT_TRUE(SerialRemote::isTokenPath("/debug/remote-token", true));
  EXPECT_FALSE(SerialRemote::isTokenPath("/debug/logs", true));
  EXPECT_FALSE(SerialRemote::isTokenPath("/", true));
}
