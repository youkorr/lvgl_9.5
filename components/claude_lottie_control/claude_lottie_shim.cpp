// Shim providing tvg_lottie_animation_assign() and tvg_lottie_animation_gen_slot()
// which LVGL 9.5's bundled thorvg_capi.cpp does NOT export (even though the
// underlying C++ class tvg::LottieAnimation::{assign,gen(slot)} is compiled
// into the thorvg library — we know because tvg_lottie_animation_new() links
// successfully, and it calls LottieAnimation::gen() from the same TU).
//
// We forward-declare tvg::LottieAnimation with just the two public methods we
// need and call them directly. Since both are non-virtual, their symbols are
// resolved by standard Itanium C++ name mangling at link time.

#ifdef USE_ESP32

#include <lvgl.h>

#if LV_USE_LOTTIE

#include <cstdint>
#include <src/libs/thorvg/thorvg_capi.h>

namespace tvg {

// Matches the real thorvg enum layout (uint8_t underlying type, Success == 0).
enum class Result : uint8_t {
  Success = 0,
  InvalidArguments,
  InsufficientCondition,
  FailedAllocation,
  MemoryCorruption,
  NonSupport,
  Unknown,
};

// Minimal forward declaration of tvg::LottieAnimation. We don't need the full
// class layout because we only call non-virtual member functions through an
// already-constructed object — the `this` pointer is passed in and the
// compiler emits a direct call to the mangled symbol. The real class inherits
// from tvg::Animation, but for linkage purposes that's irrelevant.
class LottieAnimation {
 public:
  uint32_t gen(const char *slot) noexcept;
  Result assign(const char *layer, uint32_t ix, const char *var, float val);
};

}  // namespace tvg

// --- C wrappers matching the signatures expected by our header's forward
// declarations (Tvg_Animation * handle, LVGL-bundled convention). ---

extern "C" uint32_t tvg_lottie_animation_gen_slot(Tvg_Animation *animation, const char *slot) {
  if (animation == nullptr || slot == nullptr) return 0;
  return reinterpret_cast<tvg::LottieAnimation *>(animation)->gen(slot);
}

extern "C" Tvg_Result tvg_lottie_animation_assign(Tvg_Animation *animation, const char *layer,
                                                   uint32_t ix, const char *var, float val) {
  if (animation == nullptr || layer == nullptr || var == nullptr) {
    return TVG_RESULT_INVALID_ARGUMENT;
  }
  auto r = reinterpret_cast<tvg::LottieAnimation *>(animation)->assign(layer, ix, var, val);
  return static_cast<Tvg_Result>(r);
}

#endif  // LV_USE_LOTTIE
#endif  // USE_ESP32
