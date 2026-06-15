/**
 * @file src/pyrowave/pyrowave_vk.h
 * @brief Minimal Vulkan context owner for the PyroWave GPU codec.
 *
 * PyroWave needs a live Vulkan device plus exactly one VMA `vk_allocator`
 * singleton (see third-party/pyrowave/README.md "Integration contract"). Sunshine
 * has no general-purpose Vulkan device exposed to the encoder layer, so we create
 * and own a self-contained one here. The encoder (and, by analogy, the
 * moonlight-qt decoder) drives PyroWave on this context.
 *
 * Scope note: this is the foundation for Milestone 2. It deliberately contains
 * no Sunshine capture/avcodec coupling. Built only when SUNSHINE_ENABLE_PYROWAVE.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>

// Public headers from the vendored pyrowave static library (PUBLIC include dirs
// propagate through the linked `pyrowave` target).
#include <vulkan/vulkan_raii.hpp>
#include "vk/vk_allocator.h"

namespace pyrowave_vk {

  /**
   * @brief Required/optional Vulkan device features the codec relies on.
   */
  struct device_caps_t {
    bool shader_float16 = false;  ///< Enables the faster *_fp16 shader paths.
    bool timeline_semaphore = false;
    bool ycbcr_conversion = false;
    bool dmabuf_import = false;  ///< DRM-PRIME dma-buf import (zero-copy capture) usable.
    uint32_t compute_queue_family = UINT32_MAX;
  };

  /**
   * @brief A VkImage backed by an imported DRM-PRIME dma-buf (a zero-copy capture
   *        frame). RAII members free themselves; keep alive while the GPU uses it.
   */
  struct imported_image {
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::Image image = nullptr;
    vk::raii::ImageView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    vk::Format format = vk::Format::eUndefined;

    explicit operator bool() const {
      return *image != VK_NULL_HANDLE;
    }
  };

  /**
   * @brief Owns a Vulkan instance/device dedicated to PyroWave plus the VMA
   *        singleton the codec requires.
   *
   * Lifetime: construct once before any PyroWave::Encoder/Decoder and keep alive
   * for their entire lifetime. The `vk_allocator` is a process-global singleton,
   * so at most one `context` may exist per process.
   */
  class context {
  public:
    /**
     * @brief Create the Vulkan context.
     * @param prefer_luid Optional adapter LUID/UUID to match the capture GPU
     *        (so encoder input can later be imported without a cross-GPU copy).
     * @return nullptr on failure (no suitable device, missing extensions).
     */
    static std::unique_ptr<context> create(const uint8_t *prefer_uuid = nullptr);

    ~context();

    context(const context &) = delete;
    context &operator=(const context &) = delete;

    vk::raii::PhysicalDevice &physical_device() {
      return phys_dev;
    }

    vk::raii::Device &device() {
      return dev;
    }

    const device_caps_t &caps() const {
      return caps_;
    }

    /// The compute queue PyroWave command buffers are submitted on.
    vk::raii::Queue &queue() {
      return compute_queue;
    }

    /**
     * @brief Import a single-plane DRM-PRIME (dma-buf) capture frame as a sampled
     *        VkImage, zero-copy. Internally dup()s `fd`, so the caller keeps
     *        ownership of the passed fd. Returns an empty result (operator bool
     *        == false) on failure; details are logged.
     *
     * The imported image starts in VK_IMAGE_LAYOUT_UNDEFINED and is owned by
     * VK_QUEUE_FAMILY_FOREIGN_EXT; the caller must barrier it into a usable
     * layout/queue before sampling.
     *
     * @param fd       DRM-PRIME fd of the buffer (sd.fds[0]).
     * @param fourcc   DRM fourcc (sd.fourcc), e.g. DRM_FORMAT_XRGB8888.
     * @param modifier DRM format modifier (sd.modifier).
     * @param width,height  Frame dimensions.
     * @param pitch    Row stride in bytes (sd.pitches[0]).
     * @param offset   Plane byte offset (sd.offsets[0]).
     */
    imported_image import_dmabuf(int fd, uint32_t fourcc, uint64_t modifier,
                                 uint32_t width, uint32_t height,
                                 uint32_t pitch, uint32_t offset);

  private:
    context() = default;

    vk::raii::Context ctx {};
    vk::raii::Instance inst = nullptr;
    vk::raii::PhysicalDevice phys_dev = nullptr;
    vk::raii::Device dev = nullptr;
    vk::raii::Queue compute_queue = nullptr;
    device_caps_t caps_;

    // The VMA singleton required by the vendored allocation layer. Must outlive
    // every PyroWave object; destroyed last.
    std::optional<vk_allocator> allocator;
  };

}  // namespace pyrowave_vk
