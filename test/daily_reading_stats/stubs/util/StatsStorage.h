#pragma once
#include <string>
// Identity: these suites test the folders' contents; test/stats_storage tests the per-reader paths.
namespace StatsStorage {
inline std::string folderToRead(const std::string& legacyFolder) { return legacyFolder; }
inline std::string adoptFolder(const std::string& legacyFolder) { return legacyFolder; }
}  // namespace StatsStorage
