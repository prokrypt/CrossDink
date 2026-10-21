#pragma once
#include <map>
#include <string>
#include <vector>

struct StorageStub {
  std::map<std::string, std::vector<unsigned char>> files;
  bool exists(const char* path) const { return files.contains(path); }
};
inline StorageStub Storage;
