#pragma once

#include <string>
#include <utility>

namespace lsm {

// Result of an operation that can fail. Cheap to copy when OK.
class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status() = default;
  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") { return Status(Code::kNotFound, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(Code::kCorruption, std::move(msg)); }
  static Status IOError(std::string msg) { return Status(Code::kIOError, std::move(msg)); }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  Code code() const { return code_; }
  const std::string& message() const { return msg_; }
  std::string ToString() const;

 private:
  Status(Code c, std::string msg) : code_(c), msg_(std::move(msg)) {}
  Code code_ = Code::kOk;
  std::string msg_;
};

inline std::string Status::ToString() const {
  switch (code_) {
    case Code::kOk: return "OK";
    case Code::kNotFound: return "NotFound: " + msg_;
    case Code::kCorruption: return "Corruption: " + msg_;
    case Code::kIOError: return "IOError: " + msg_;
    case Code::kInvalidArgument: return "InvalidArgument: " + msg_;
  }
  return "Unknown";
}

}  // namespace lsm
