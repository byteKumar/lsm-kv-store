#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace lsm::test {

// A fresh directory under $TMPDIR, deleted when the object goes away.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "lsmtest-XXXXXX").string();
    char* p = ::mkdtemp(tmpl.data());
    if (p == nullptr) std::abort();
    path_ = p;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::string& path() const { return path_; }
  std::string File(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

}  // namespace lsm::test
