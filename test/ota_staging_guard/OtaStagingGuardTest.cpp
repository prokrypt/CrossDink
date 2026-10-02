#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string sourceFile(const char* path) {
  std::ifstream source(path);
  return {std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
}

TEST(OtaStagingGuard, ValidatesBoardTaggedImageBeforeFlashCanStart) {
  const std::string source = sourceFile(OTA_UPDATER_SOURCE_PATH);
  const std::string flasher = sourceFile(FIRMWARE_FLASHER_SOURCE_PATH);
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(flasher.empty());

  // The old streaming path could start esp_ota before a board tag in a later
  // .rodata chunk was found. Keep direct OTA writes out of OtaUpdater.
  EXPECT_EQ(source.find("esp_ota_begin("), std::string::npos);
  EXPECT_EQ(source.find("esp_ota_write("), std::string::npos);

  const size_t validation = source.find("firmware_flash::validateOpenImageFile(stagedFile");
  const size_t rejection = source.find("validationResult == firmware_flash::Result::BAD_CHIP ||");
  const size_t flash = source.find("firmware_flash::flashValidatedFile(");
  ASSERT_NE(validation, std::string::npos);
  ASSERT_NE(rejection, std::string::npos);
  ASSERT_NE(flash, std::string::npos);
  EXPECT_LT(validation, rejection);
  EXPECT_LT(rejection, flash);

  // The shared open-file path must not reopen by pathname after validation;
  // otherwise a staged file could be replaced before its first erase/write.
  const size_t flashValidated = flasher.find("Result flashValidatedFileImpl(HalFile& file");
  const size_t pathFlasher = flasher.find("Result flashFromSdPath(");
  ASSERT_NE(flashValidated, std::string::npos);
  ASSERT_NE(pathFlasher, std::string::npos);
  EXPECT_EQ(flasher.substr(flashValidated, pathFlasher - flashValidated).find("Storage.openFileForRead"),
            std::string::npos);

  // The SD path checks only the header up front; the full image (checksum,
  // SHA-256, board tag) is verified over the bytes as they are written, and
  // the new slot is activated only after that verification passes.
  const std::string sharedPathFlasher = flasher.substr(pathFlasher);
  const size_t sharedHeader = sharedPathFlasher.find("checkImageHeader(file");
  const size_t sharedFlash = sharedPathFlasher.find("flashValidatedFile(file");
  ASSERT_NE(sharedHeader, std::string::npos);
  ASSERT_NE(sharedFlash, std::string::npos);
  EXPECT_LT(sharedHeader, sharedFlash);

  const std::string validatedFlasher = flasher.substr(flashValidated, pathFlasher - flashValidated);
  const size_t streamVerify = validatedFlasher.find("verifier.finish()");
  const size_t activate = validatedFlasher.find("ota_boot::switchTo(dest)");
  ASSERT_NE(streamVerify, std::string::npos);
  ASSERT_NE(activate, std::string::npos);
  EXPECT_LT(streamVerify, activate);
}

}  // namespace
