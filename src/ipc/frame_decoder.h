#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace nativekit {

class FrameDecoder {
 public:
  static constexpr std::uint32_t kMaximumFrameSize = 16 * 1024 * 1024;

  std::vector<std::vector<std::uint8_t>> push(
      const std::uint8_t* data,
      std::size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t offset = 0;
    while (buffer_.size() - offset >= sizeof(std::uint32_t)) {
      const std::uint32_t length =
          static_cast<std::uint32_t>(buffer_[offset]) |
          (static_cast<std::uint32_t>(buffer_[offset + 1]) << 8U) |
          (static_cast<std::uint32_t>(buffer_[offset + 2]) << 16U) |
          (static_cast<std::uint32_t>(buffer_[offset + 3]) << 24U);
      if (length == 0 || length > kMaximumFrameSize) {
        throw std::runtime_error("secure channel frame length is invalid");
      }
      const std::size_t frame_end = offset + sizeof(std::uint32_t) + length;
      if (frame_end > buffer_.size()) break;
      frames.emplace_back(
          buffer_.begin() + offset + sizeof(std::uint32_t),
          buffer_.begin() + frame_end);
      offset = frame_end;
    }
    if (offset != 0) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
    }
    if (buffer_.size() > kMaximumFrameSize + sizeof(std::uint32_t)) {
      throw std::runtime_error("secure channel buffer exceeds the limit");
    }
    return frames;
  }

  void reset() { buffer_.clear(); }
  bool has_pending_data() const { return !buffer_.empty(); }

 private:
  std::vector<std::uint8_t> buffer_;
};

}  // namespace nativekit
