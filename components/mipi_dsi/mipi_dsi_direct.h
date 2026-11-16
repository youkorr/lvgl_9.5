#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace esphome::display {
class Display;
}

namespace esphome::mipi_dsi {

// One producer owns this state. Only frame_complete() runs from the ISR.
// A draw_bitmap on a panel buffer changes the driver's NEXT buffer; its
// color-trans-done callback is immediate, not a scan-out completion.
class ScanoutBuffers {
 public:
  void init(void *const buffers[3]) {
    for (int i = 0; i < 3; i++) {
      this->buffers_[i] = buffers[i];
      this->state_[i] = i == 0 ? CURRENT : FREE;
    }
    this->current_ = 0;  // ESP-IDF starts DPI on fb[0].
    this->frames_.store(0, std::memory_order_relaxed);
  }
  void frame_complete() { this->frames_.fetch_add(1, std::memory_order_release); }
  void *acquire() {
    const uint32_t now = this->frames_.load(std::memory_order_acquire);
    for (int i = 0; i < 3; i++) {
      if (this->state_[i] == RETIRED && static_cast<int32_t>(now - this->until_[i]) >= 0)
        this->state_[i] = FREE;
      if (this->state_[i] == FREE) {
        this->state_[i] = LEASED;
        return this->buffers_[i];
      }
    }
    return nullptr;
  }
  bool leased(void *buffer) const {
    const int i = this->index_(buffer);
    return i >= 0 && this->state_[i] == LEASED;
  }
  void cancel(void *buffer) {
    const int i = this->index_(buffer);
    if (i >= 0 && this->state_[i] == LEASED)
      this->state_[i] = FREE;
  }
  // Called AFTER draw_bitmap has returned successfully. An ISR may have
  // selected the previous buffer just before the swap and call us after it.
  // Two further completions cover that in-flight ISR and one selection of
  // the replacement. Three buffers let the producer continue meanwhile.
  bool presented(void *buffer) {
    const int i = this->index_(buffer);
    if (i < 0 || this->state_[i] != LEASED)
      return false;
    this->state_[this->current_] = RETIRED;
    this->until_[this->current_] = this->frames_.load(std::memory_order_acquire) + 2;
    this->current_ = i;
    this->state_[i] = CURRENT;
    return true;
  }

 private:
  enum State : uint8_t { FREE, LEASED, CURRENT, RETIRED };
  int index_(void *buffer) const {
    for (int i = 0; i < 3; i++)
      if (this->buffers_[i] == buffer)
        return i;
    return -1;
  }
  void *buffers_[3]{};
  State state_[3]{};
  uint32_t until_[3]{};
  int current_{0};
  std::atomic<uint32_t> frames_{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "The scan-out counter must be ISR-safe");

// Optional ABI for an exclusive external producer (Portall, no LVGL/writer).
// acquire cleans/invalidates CPU cache before a DMA decoder writes the buffer.
// present accepts only a buffer acquired here. cancel releases a failed decode.
struct DirectPanel {
  display::Display *display{};
  void *instance{};
  int width{}, height{}, bytes_per_pixel{};
  size_t buffer_size{};
  uint8_t *(*acquire)(void *instance, uint32_t timeout_ms){};
  bool (*present)(void *instance, uint8_t *buffer){};
  void (*cancel)(void *instance, uint8_t *buffer){};
};
inline DirectPanel g_direct_panel{};

}  // namespace esphome::mipi_dsi
