/**
 * @file src/pyrowave/pyrowave_encode.h
 * @brief Sunshine encode device + session for the PyroWave GPU wavelet codec.
 *
 * PyroWave is a Vulkan compute/fragment codec, so it does not use the avcodec
 * path. This device mirrors the role of platf::nvenc_encode_device_t: it takes a
 * captured platf::img_t in convert(), produces the encoded bitstream, and the
 * session hands one contiguous packet per frame to Sunshine's RTP layer (which
 * fragments it; the decoder reassembles and parses PyroWave's self-delimiting
 * block headers).
 *
 * Milestone 2 scaffold. The codec-driving core is faithful to WiVRn's
 * server/encoder/video_encoder_pyrowave.cpp, but this has NOT been compiled
 * (no Vulkan SDK in the authoring environment). Build with
 * -DSUNSHINE_ENABLE_PYROWAVE=ON on a real host. Points needing validation are
 * marked TODO. The colour-conversion upload is a CPU-staging first version;
 * zero-copy GPU interop with the capture backend is a later optimisation.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/platform/common.h"
#include "src/video.h"

#include "pyrowave_vk.h"

// Vendored PyroWave public headers (include dirs propagate from the linked lib).
#include "pyrowave/pyrowave_encoder.h"
#include "vk/allocation.h"

namespace pyrowave_enc {

  /**
   * @brief Encoded output for a single frame.
   */
  struct encoded_frame {
    std::vector<uint8_t> data;
    int64_t frame_index = 0;
    bool idr = true;  ///< PyroWave is intra-only: every frame is an IDR.
  };

  /**
   * @brief encode_device_t implementation backed by PyroWave::Encoder.
   *
   * Owns the Vulkan input image, the host-visible meta/bitstream buffers, and a
   * command buffer/fence. convert() uploads + records + submits the GPU encode;
   * encode_frame() waits, maps the results and packetizes.
   */
  class pyrowave_encode_device_t: public platf::encode_device_t {
  public:
    /**
     * @brief Construct an encode device.
     * @param ctx Shared Vulkan context (owns the device + VMA singleton).
     * @param width,height Stream dimensions.
     * @param bitrate_bps Target bitrate; per-frame byte budget = bitrate/(fps*8).
     * @param fps Frame rate.
     * @return nullptr on failure.
     */
    static std::unique_ptr<pyrowave_encode_device_t> create(
      std::shared_ptr<pyrowave_vk::context> ctx,
      int width,
      int height,
      int64_t bitrate_bps,
      int fps,
      const video::sunshine_colorspace_t &colorspace);

    ~pyrowave_encode_device_t() override;

    /// Upload the captured image, record and submit the GPU encode.
    int convert(platf::img_t &img) override;

    /// Wait for the GPU, packetize, and return the frame bitstream.
    encoded_frame encode_frame(int64_t frame_index);

  private:
    pyrowave_encode_device_t() = default;

    bool init(int width, int height, int64_t bitrate_bps, int fps);
    bool upload_image(platf::img_t &img);  ///< memcpy the BGRA capture into the source staging buffer.
    bool ensure_source(uint32_t src_w, uint32_t src_h);  ///< (re)create the GPU source image at capture res.
    bool ensure_cursor(uint32_t w, uint32_t h);  ///< (re)create the cursor image at the cursor size.

    std::shared_ptr<pyrowave_vk::context> ctx_;
    std::unique_ptr<PyroWave::Encoder> enc_;

    int width_ = 0;
    int height_ = 0;
    size_t target_size_ = 0;  ///< per-frame byte budget

    // Vulkan input planes (separate R8 images: Y full-res, Cb/Cr half-res for
    // 4:2:0) + their views, passed as the encoder's ViewBuffers.
    image_allocation img_y_, img_cb_, img_cr_;
    vk::raii::ImageView view_y_ = nullptr;
    vk::raii::ImageView view_cb_ = nullptr;
    vk::raii::ImageView view_cr_ = nullptr;

    // Host-visible output buffers (meta + bitstream), with staging fallbacks.
    buffer_allocation meta_buf_, data_buf_;
    buffer_allocation meta_staging_, data_staging_;

    // GPU colour conversion: the captured BGRA frame is uploaded once to a sampled
    // image, then a compute shader (rgb2yuv) writes the three R8 planes with a
    // bilinear downscale to the encode resolution. Replaces the per-pixel CPU path.
    buffer_allocation bgra_staging_;        ///< host-visible BGRA upload buffer (capture res)
    image_allocation img_src_;              ///< sampled B8G8R8A8 source image (capture res)
    vk::raii::ImageView view_src_ = nullptr;
    vk::raii::Sampler sampler_ = nullptr;
    vk::raii::DescriptorSetLayout conv_dsl_ = nullptr;
    vk::raii::PipelineLayout conv_pl_ = nullptr;
    vk::raii::Pipeline conv_pipe_ = nullptr;
    vk::raii::DescriptorPool conv_dpool_ = nullptr;
    vk::raii::DescriptorSet conv_dset_ = nullptr;
    uint32_t src_w_ = 0, src_h_ = 0;        ///< current img_src_ dimensions

    // Zero-copy path: the current frame's imported DRM-PRIME capture image. Held as
    // a member so it outlives the in-flight GPU work (replaced next convert(), after
    // encode_frame() has waited the fence). rgb2yuv samples cur_import_.view instead
    // of img_src_ when this path is active.
    pyrowave_vk::imported_image cur_import_;
    bool logged_first_dmabuf_ = false;
    bool logged_import_fail_ = false;

    // Cursor compositing (the dma-buf screen plane excludes the hardware cursor).
    image_allocation cursor_img_;            ///< premultiplied BGRA cursor, sampled in rgb2yuv
    vk::raii::ImageView cursor_view_ = nullptr;
    buffer_allocation cursor_staging_;       ///< host-visible cursor upload buffer
    uint32_t cursor_w_ = 0, cursor_h_ = 0;
    bool cursor_initialized_ = false;

    vk::raii::CommandPool cmd_pool_ = nullptr;
    vk::raii::CommandBuffer cmd_ = nullptr;
    vk::raii::Fence fence_ = nullptr;
    bool image_initialized_ = false;
    bool src_initialized_ = false;
    bool busy_ = false;
    bool logged_first_convert_ = false;
    bool logged_first_encode_ = false;
    uint64_t validate_log_count_ = 0;  ///< diagnostic: frames passed through bitstream validation
  };

}  // namespace pyrowave_enc
