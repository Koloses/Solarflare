/**
 * @file src/pyrowave/pyrowave_encode.cpp
 * @brief Implementation of the PyroWave encode device. See header for status.
 *
 * Flow per frame:
 *   convert(img): BGRA -> YCbCr 4:2:0 on CPU -> staging buffer -> upload to the
 *                 three R8 plane images -> record PyroWave::Encoder::encode ->
 *                 submit on the compute queue with a fence.
 *   encode_frame(): wait fence, map meta + bitstream, packetize into one
 *                 contiguous self-delimiting blob (Sunshine's RTP layer fragments
 *                 it; the decoder reassembles and walks the block headers).
 */
#include "pyrowave_encode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "src/logging.h"

// Zero-copy capture: the VRAM capture path delivers an egl::img_descriptor_t that
// carries a DRM-PRIME dma-buf descriptor (sd). Linux-only.
#if defined(__linux__)
  #include "src/platform/linux/graphics.h"
#endif

// Vendored codec: shader-module loader (compiled SPIR-V map).
#include "pyrowave/pyrowave_common.h"

using namespace std::literals;

namespace pyrowave_enc {

  namespace {

    // Push constants for rgb2yuv.comp: encode (luma) resolution + its reciprocal.
    struct ConvPush {
      int32_t luma_w;
      int32_t luma_h;
      float inv_w;
      float inv_h;
      int32_t flip_y;
      int32_t have_cursor;
      float cursor_x, cursor_y;   // normalized over the source frame (top-down)
      float cursor_w, cursor_h;
    };

    // Create a single-plane R8 image usable as sampled + storage + transfer dst.
    image_allocation make_r8_image(vk::raii::Device &device, uint32_t w, uint32_t h, const char *name) {
      vk::ImageCreateInfo info {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Unorm,
        .extent = {.width = w, .height = h, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                 vk::ImageUsageFlagBits::eTransferDst,
      };
      return image_allocation(device, info, {.usage = VMA_MEMORY_USAGE_AUTO}, name);
    }

    vk::raii::ImageView make_view(vk::raii::Device &device, vk::Image img) {
      return device.createImageView(vk::ImageViewCreateInfo {
        .image = img,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Unorm,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1},
      });
    }

    // Conditional replenishment sentinels for the per-block hash table.
    constexpr uint64_t kHashNever = 0;  ///< block never sent (forces a send)
    constexpr uint64_t kHashEmpty = 1;  ///< block last coded as an explicit zero block

    // FNV-1a over a packed block packet with the 3 sequence bits masked out
    // (they change every frame). The high bit is forced so real hashes never
    // collide with the sentinels above.
    uint64_t hash_block_packet(const uint8_t *p, size_t n) {
      uint64_t h = 1469598103934665603ull;
      for (size_t i = 0; i < n; ++i) {
        uint8_t b = p[i];
        if (i == 3) {
          b &= uint8_t(~0x70);  // BitstreamHeader byte 3, bits 4-6 = sequence
        }
        h ^= b;
        h *= 1099511628211ull;
      }
      return h | 0x8000000000000000ull;
    }

    buffer_allocation make_host_buffer(vk::raii::Device &device, size_t size, vk::BufferUsageFlags usage, const char *name) {
      return buffer_allocation(
        device,
        {.size = size, .usage = usage},
        {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
         .usage = VMA_MEMORY_USAGE_AUTO},
        name);
    }

  }  // namespace

  std::unique_ptr<pyrowave_encode_device_t> pyrowave_encode_device_t::create(
    std::shared_ptr<pyrowave_vk::context> ctx,
    int width,
    int height,
    int64_t bitrate_bps,
    int fps,
    const video::sunshine_colorspace_t &colorspace,
    size_t shard_payload_size,
    int quality_bias,
    int refresh_interval) {
    auto self = std::unique_ptr<pyrowave_encode_device_t>(new pyrowave_encode_device_t());
    self->ctx_ = std::move(ctx);
    self->colorspace = colorspace;
    // Alignment needs a sane, word-aligned shard payload (first shard also
    // carries the 8-byte frame header, so require some minimum room).
    if (shard_payload_size >= 64 && shard_payload_size % 4 == 0) {
      self->shard_payload_size_ = shard_payload_size;
    }
    self->quality_bias_ = std::clamp(quality_bias, 0, 3);
    self->refresh_interval_ = std::clamp(refresh_interval, 0, 255);
    if (!self->init(width, height, bitrate_bps, std::max(1, fps))) {
      return nullptr;
    }
    return self;
  }

  bool pyrowave_encode_device_t::init(int width, int height, int64_t bitrate_bps, int fps) {
    try {
      width_ = width;
      height_ = height;
      target_size_ = (size_t) std::max<int64_t>(1, bitrate_bps / (int64_t(fps) * 8));

      auto &device = ctx_->device();

      BOOST_LOG(info) << "pyrowave: creating Encoder " << width << "x" << height;
      enc_ = std::make_unique<PyroWave::Encoder>(
        ctx_->physical_device(), device, width, height, PyroWave::ChromaSubsampling::Chroma420);
      enc_->set_quality_bias(quality_bias_);
      block_hashes_.assign(size_t(enc_->block_count_32x32), kHashNever);
      BOOST_LOG(info) << "pyrowave: Encoder created";

      // Three R8 plane images (Y full-res, Cb/Cr half-res for 4:2:0).
      uint32_t cw = (uint32_t(width) + 1) / 2;
      uint32_t ch = (uint32_t(height) + 1) / 2;
      img_y_ = make_r8_image(device, width, height, "pyrowave Y");
      img_cb_ = make_r8_image(device, cw, ch, "pyrowave Cb");
      img_cr_ = make_r8_image(device, cw, ch, "pyrowave Cr");
      view_y_ = make_view(device, img_y_);
      view_cb_ = make_view(device, img_cb_);
      view_cr_ = make_view(device, img_cr_);

      size_t meta_size = enc_->get_meta_required_size();
      meta_buf_ = make_host_buffer(device, meta_size,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc, "pyrowave meta");

      // Bitstream buffer must hold the WORST-CASE frame, not the rate-control TARGET:
      // the target is only the payload budget, but the packed bitstream also carries
      // a per-32x32-block header (BitstreamHeader) plus a sequence header, and the
      // RDO can overshoot the target on complex frames. Undersizing this lets the GPU
      // packer overrun the buffer -> garbled-but-parseable coefficients (intermittent
      // pixelated noise). Size generously: target + all block headers + a floor.
      size_t blocks_32 = size_t((width + 31) / 32) * size_t((height + 31) / 32);
      size_t header_overhead = blocks_32 * 64 + 4096;  // 64B/block headroom + seq header
      // target*4: headroom for the adaptive replenishment budget (scale <= 2)
      // plus RDO overshoot on complex frames.
      size_t data_size = std::max<size_t>(target_size_ * 4, size_t(width) * height / 2)
                         + header_overhead + 2 * meta_size;
      data_buf_ = make_host_buffer(device, data_size,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc, "pyrowave data");

      // Staging fallbacks if the device buffers are not host-visible.
      if (!(meta_buf_.properties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        meta_staging_ = make_host_buffer(device, meta_buf_.info().size, vk::BufferUsageFlagBits::eTransferDst, "pyrowave meta staging");
      }
      if (!(data_buf_.properties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        data_staging_ = make_host_buffer(device, data_buf_.info().size, vk::BufferUsageFlagBits::eTransferDst, "pyrowave data staging");
      }

      // --- GPU colour-conversion pipeline (rgb2yuv compute) -----------------
      // A linear, clamp-to-edge sampler for the bilinear downscale.
      sampler_ = vk::raii::Sampler(device, vk::SamplerCreateInfo {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge});

      std::array<vk::DescriptorSetLayoutBinding, 6> binds {{
        {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1, .descriptorType = vk::DescriptorType::eSampler,      .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 4, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 5, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
      }};
      conv_dsl_ = vk::raii::DescriptorSetLayout(device, vk::DescriptorSetLayoutCreateInfo {
        .bindingCount = (uint32_t) binds.size(), .pBindings = binds.data()});

      vk::PushConstantRange pcr {.stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(ConvPush)};
      conv_pl_ = vk::raii::PipelineLayout(device, vk::PipelineLayoutCreateInfo {
        .setLayoutCount = 1, .pSetLayouts = &*conv_dsl_, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr});

      auto conv_shader = PyroWave::load_shader(device, "rgb2yuv");
      vk::PipelineShaderStageCreateInfo conv_stage {
        .stage = vk::ShaderStageFlagBits::eCompute, .module = *conv_shader, .pName = "main"};
      conv_pipe_ = vk::raii::Pipeline(device, nullptr, vk::ComputePipelineCreateInfo {
        .stage = conv_stage, .layout = *conv_pl_});

      std::array<vk::DescriptorPoolSize, 3> psz {{
        {.type = vk::DescriptorType::eSampledImage, .descriptorCount = 2},  // source + cursor
        {.type = vk::DescriptorType::eSampler,      .descriptorCount = 1},
        {.type = vk::DescriptorType::eStorageImage, .descriptorCount = 3},
      }};
      conv_dpool_ = vk::raii::DescriptorPool(device, vk::DescriptorPoolCreateInfo {
        .maxSets = 1, .poolSizeCount = (uint32_t) psz.size(), .pPoolSizes = psz.data()});
      conv_dset_ = std::move(vk::raii::DescriptorSets(device, vk::DescriptorSetAllocateInfo {
        .descriptorPool = *conv_dpool_, .descriptorSetCount = 1, .pSetLayouts = &*conv_dsl_}).front());

      // Static bindings: sampler + the three output plane storage images. The
      // source image (binding 0) is written lazily by ensure_source().
      vk::DescriptorImageInfo samp_info {.sampler = *sampler_};
      vk::DescriptorImageInfo y_info {.imageView = *view_y_, .imageLayout = vk::ImageLayout::eGeneral};
      vk::DescriptorImageInfo cb_info {.imageView = *view_cb_, .imageLayout = vk::ImageLayout::eGeneral};
      vk::DescriptorImageInfo cr_info {.imageView = *view_cr_, .imageLayout = vk::ImageLayout::eGeneral};
      std::array<vk::WriteDescriptorSet, 4> writes {{
        {.dstSet = *conv_dset_, .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampler,      .pImageInfo = &samp_info},
        {.dstSet = *conv_dset_, .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &y_info},
        {.dstSet = *conv_dset_, .dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cb_info},
        {.dstSet = *conv_dset_, .dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cr_info},
      }};
      device.updateDescriptorSets(writes, {});

      // Default 1x1 cursor image so binding 5 is always valid (re-created at the real
      // cursor size on the first frame that carries one).
      ensure_cursor(1, 1);

      // Command buffer + fence.
      cmd_pool_ = vk::raii::CommandPool(device, vk::CommandPoolCreateInfo {
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = ctx_->caps().compute_queue_family,
      });
      cmd_ = std::move(device.allocateCommandBuffers(vk::CommandBufferAllocateInfo {
        .commandPool = *cmd_pool_, .commandBufferCount = 1})[0]);
      fence_ = vk::raii::Fence(device, vk::FenceCreateInfo {});

      BOOST_LOG(info) << "pyrowave: encode device ready " << width << "x" << height
                      << " target/frame=" << target_size_ << " bytes"
                      << " precision=" << PyroWave::Configuration::get().get_precision()
                      << " quality_bias=" << quality_bias_
                      << " refresh_interval=" << refresh_interval_
                      << " shard_payload=" << shard_payload_size_
                      << " fp16_math=" << (std::getenv("PYROWAVE_NO_FP16_MATH") ? "OFF(forced fp32)" : "on");
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "pyrowave: encode device init failed: " << e.what();
      return false;
    }
  }

  bool pyrowave_encode_device_t::ensure_source(uint32_t src_w, uint32_t src_h) {
    if (img_src_ && src_w == src_w_ && src_h == src_h_) {
      return true;
    }
    auto &device = ctx_->device();
    vk::ImageCreateInfo info {
      .imageType = vk::ImageType::e2D,
      .format = vk::Format::eB8G8R8A8Unorm,
      .extent = {.width = src_w, .height = src_h, .depth = 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
    };
    img_src_ = image_allocation(device, info, {.usage = VMA_MEMORY_USAGE_AUTO}, "pyrowave src BGRA");
    view_src_ = device.createImageView(vk::ImageViewCreateInfo {
      .image = img_src_, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eB8G8R8A8Unorm,
      .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
    bgra_staging_ = make_host_buffer(device, size_t(src_w) * src_h * 4,
      vk::BufferUsageFlagBits::eTransferSrc, "pyrowave bgra staging");
    src_w_ = src_w;
    src_h_ = src_h;
    src_initialized_ = false;

    // Point binding 0 (the sampled source) at the new image.
    vk::DescriptorImageInfo src_info {.imageView = *view_src_, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet w {.dstSet = *conv_dset_, .dstBinding = 0, .descriptorCount = 1,
      .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &src_info};
    device.updateDescriptorSets(w, {});
    return true;
  }

  bool pyrowave_encode_device_t::ensure_cursor(uint32_t w, uint32_t h) {
    if (*cursor_view_ && w == cursor_w_ && h == cursor_h_) {
      return true;
    }
    auto &device = ctx_->device();
    vk::ImageCreateInfo info {
      .imageType = vk::ImageType::e2D,
      .format = vk::Format::eB8G8R8A8Unorm,  // cursor is premultiplied BGRA
      .extent = {.width = w, .height = h, .depth = 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
    };
    cursor_img_ = image_allocation(device, info, {.usage = VMA_MEMORY_USAGE_AUTO}, "pyrowave cursor");
    cursor_view_ = device.createImageView(vk::ImageViewCreateInfo {
      .image = cursor_img_, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eB8G8R8A8Unorm,
      .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
    cursor_staging_ = make_host_buffer(device, size_t(w) * h * 4,
      vk::BufferUsageFlagBits::eTransferSrc, "pyrowave cursor staging");
    cursor_w_ = w;
    cursor_h_ = h;
    cursor_initialized_ = false;

    // Point binding 5 (the cursor sampled image) at the new image.
    vk::DescriptorImageInfo ci {.imageView = *cursor_view_, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet w5 {.dstSet = *conv_dset_, .dstBinding = 5, .descriptorCount = 1,
      .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &ci};
    device.updateDescriptorSets(w5, {});
    return true;
  }

  bool pyrowave_encode_device_t::upload_image(platf::img_t &img) {
    // Linear copy of the captured BGRA frame into the host-visible staging buffer.
    // No per-pixel maths: BGRA->YCbCr + scaling happens on the GPU (rgb2yuv.comp).
    if (!img.data) {
      return false;
    }
    auto *dst = static_cast<uint8_t *>(bgra_staging_.map());
    const size_t row_bytes = size_t(src_w_) * 4;
    for (uint32_t y = 0; y < src_h_; ++y) {
      std::memcpy(dst + size_t(y) * row_bytes, img.data + size_t(y) * img.row_pitch, row_bytes);
    }
    return true;
  }

  int pyrowave_encode_device_t::convert(platf::img_t &img) {
    try {
      if (!logged_first_convert_) {
        logged_first_convert_ = true;
        BOOST_LOG(info) << "pyrowave: first convert() " << img.width << "x" << img.height
                        << " pixel_pitch=" << img.pixel_pitch << " row_pitch=" << img.row_pitch
                        << " data=" << (void *) img.data;
      }
      auto &device = ctx_->device();

      // --- Source: zero-copy dma-buf import (preferred) or system-mem upload -----
      int32_t flip_y = 0;
      int32_t have_cursor = 0;
      float cur_x = 0, cur_y = 0, cur_w = 0, cur_h = 0;  // cursor rect, normalized over source
      bool used_dmabuf = false;
      bool is_vram = false;  // capture frame is a VRAM (DRM-PRIME) descriptor
#if defined(__linux__)
      egl::img_descriptor_t *desc =
        ctx_->caps().dmabuf_import ? dynamic_cast<egl::img_descriptor_t *>(&img) : nullptr;
      if (desc) {
        // VRAM capture frame: there is NO system-memory fallback for it (img.data
        // is null), so this branch must fully handle it.
        is_vram = true;
        if (desc->sd.fds[0] < 0) {
          // Empty/dummy frame: Sunshine's setup-validation image (nothing captured
          // yet) or a transient capture miss. Report success (no-op) so session
          // setup proceeds; at runtime we just skip the frame.
          return 0;
        }
        auto imported = ctx_->import_dmabuf(
          desc->sd.fds[0], desc->sd.fourcc, desc->sd.modifier,
          (uint32_t) desc->sd.width, (uint32_t) desc->sd.height,
          desc->sd.pitches[0], desc->sd.offsets[0]);
        if (imported) {
          if (!logged_first_dmabuf_) {
            logged_first_dmabuf_ = true;
            BOOST_LOG(info) << "pyrowave: zero-copy dma-buf capture active ("
                            << desc->sd.width << "x" << desc->sd.height
                            << " fourcc=0x" << std::hex << desc->sd.fourcc
                            << " modifier=0x" << desc->sd.modifier << std::dec
                            << " y_invert=" << desc->y_invert << ")";
          }
          cur_import_ = std::move(imported);
          flip_y = desc->y_invert ? 1 : 0;
          used_dmabuf = true;
          // Point the rgb2yuv source (binding 0) at the freshly imported image.
          vk::DescriptorImageInfo src_info {.imageView = *cur_import_.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
          vk::WriteDescriptorSet w {.dstSet = *conv_dset_, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &src_info};
          device.updateDescriptorSets(w, {});

          // Cursor: the captured screen plane excludes the hardware cursor overlay.
          // The capture layer hands it to us as premultiplied BGRA pixels (desc->buffer)
          // with screen-space position/size; stage it for compositing in rgb2yuv.
          if (desc->data && desc->src_w > 0 && desc->src_h > 0) {
            ensure_cursor((uint32_t) desc->src_w, (uint32_t) desc->src_h);
            size_t n = std::min(desc->buffer.size(), size_t(desc->src_w) * size_t(desc->src_h) * 4);
            std::memcpy(cursor_staging_.map(), desc->buffer.data(), n);
            have_cursor = 1;
            float sw = float(desc->sd.width), sh = float(desc->sd.height);
            cur_x = float(desc->x) / sw;
            cur_y = float(desc->y) / sh;
            cur_w = float(desc->src_w) / sw;
            cur_h = float(desc->src_h) / sh;
          }
        } else {
          // A VRAM frame has no .data, so there is no upload fallback for it.
          if (!logged_import_fail_) {
            logged_import_fail_ = true;
            BOOST_LOG(error) << "pyrowave: dma-buf import failed (fourcc=0x" << std::hex
                             << desc->sd.fourcc << " modifier=0x" << desc->sd.modifier << std::dec
                             << "); dropping frame";
          }
          return -1;
        }
      }
#endif
      if (!is_vram) {
        // System-memory capture path (img.data BGRA): upload + GPU rgb2yuv.
        if (!ensure_source((uint32_t) std::max(1, img.width), (uint32_t) std::max(1, img.height))) {
          return -1;
        }
        if (!upload_image(img)) {
          BOOST_LOG(warning) << "pyrowave: upload_image failed (img.data null?)";
          return -1;
        }
      }

      cmd_.reset();
      cmd_.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

      auto img_barrier = [&](vk::Image image, vk::ImageLayout oldL, vk::ImageLayout newL,
                             vk::AccessFlags src, vk::AccessFlags dst,
                             vk::PipelineStageFlags ss, vk::PipelineStageFlags ds) {
        cmd_.pipelineBarrier(ss, ds, {}, {}, {}, vk::ImageMemoryBarrier {
          .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = oldL, .newLayout = newL,
          .image = image,
          .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
      };

      // 1) Make the source image sampleable.
      if (used_dmabuf) {
#if defined(__linux__)
        // Acquire the imported frame from the foreign (compositor) queue family and
        // transition it to a sampled layout. UNDEFINED old layout is intentional:
        // for DRM-format-modifier images the pixel data is defined by the modifier
        // and is preserved across this acquire (we only read it).
        cmd_.pipelineBarrier(
          vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eComputeShader,
          {}, {}, {}, vk::ImageMemoryBarrier {
            .srcAccessMask = {},
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = ctx_->caps().compute_queue_family,
            .image = *cur_import_.image,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1}});
#endif
      } else {
        // Upload BGRA staging -> source image, then make it shader-readable.
        img_barrier(img_src_, src_initialized_ ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal,
                    src_initialized_ ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite,
                    src_initialized_ ? vk::PipelineStageFlagBits::eComputeShader : vk::PipelineStageFlagBits::eTopOfPipe,
                    vk::PipelineStageFlagBits::eTransfer);
        cmd_.copyBufferToImage(bgra_staging_, img_src_, vk::ImageLayout::eTransferDstOptimal, vk::BufferImageCopy {
          .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
          .imageExtent = {.width = src_w_, .height = src_h_, .depth = 1}});
        img_barrier(img_src_, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader);
        src_initialized_ = true;
      }

      // 1b) Upload + make sampleable the cursor image, if compositing one this frame.
      if (have_cursor) {
        img_barrier(cursor_img_, cursor_initialized_ ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal,
                    cursor_initialized_ ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite,
                    cursor_initialized_ ? vk::PipelineStageFlagBits::eComputeShader : vk::PipelineStageFlagBits::eTopOfPipe,
                    vk::PipelineStageFlagBits::eTransfer);
        cmd_.copyBufferToImage(cursor_staging_, cursor_img_, vk::ImageLayout::eTransferDstOptimal, vk::BufferImageCopy {
          .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
          .imageExtent = {.width = cursor_w_, .height = cursor_h_, .depth = 1}});
        img_barrier(cursor_img_, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader);
        cursor_initialized_ = true;
      }

      // 2) Plane images -> General for compute write.
      auto old_layout = image_initialized_ ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined;
      auto old_access = image_initialized_ ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{};
      for (vk::Image plane : {vk::Image(img_y_), vk::Image(img_cb_), vk::Image(img_cr_)}) {
        img_barrier(plane, old_layout, vk::ImageLayout::eGeneral, old_access, vk::AccessFlagBits::eShaderWrite,
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader);
      }
      image_initialized_ = true;

      // 3) Dispatch rgb2yuv (BGRA -> Y/Cb/Cr planes, bilinear scaled to encode res).
      cmd_.bindPipeline(vk::PipelineBindPoint::eCompute, *conv_pipe_);
      cmd_.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *conv_pl_, 0, *conv_dset_, {});
      ConvPush push {(int32_t) width_, (int32_t) height_, 1.0f / float(width_), 1.0f / float(height_),
                     flip_y, have_cursor, cur_x, cur_y, cur_w, cur_h};
      cmd_.pushConstants<ConvPush>(*conv_pl_, vk::ShaderStageFlagBits::eCompute, 0, push);
      cmd_.dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

      // 4) compute-write -> encoder-read on the planes.
      for (vk::Image plane : {vk::Image(img_y_), vk::Image(img_cb_), vk::Image(img_cr_)}) {
        img_barrier(plane, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader);
      }

      // 5) Record the GPU encode.
      PyroWave::Encoder::ViewBuffers views {*view_y_, *view_cb_, *view_cr_};
      PyroWave::Encoder::BitstreamBuffers buffers {
        .meta = {.buffer = meta_buf_, .offset = 0, .size = meta_buf_.info().size},
        .bitstream = {.buffer = data_buf_, .offset = 0, .size = data_buf_.info().size},
        .target_size = adaptive_target_size(),
      };
      if (!enc_->encode(cmd_, views, buffers)) {
        BOOST_LOG(error) << "pyrowave encode() failed to record";
        return -1;
      }
      // The codec increments its 3-bit sequence per encode(); mirror it so
      // synthetic (explicit-zero) block headers can carry the right sequence
      // even when no coded block is available to read it from.
      seq_mirror_ = (seq_mirror_ + 1) & 0x7;

      if (meta_staging_) {
        cmd_.copyBuffer(meta_buf_, meta_staging_, vk::BufferCopy {.size = buffers.meta.size});
      }
      // NOTE: the bitstream buffer is NOT copied here. encode_frame() first maps
      // the (tiny) meta buffer to learn the used extent, then copies only that
      // region instead of the multi-MB worst-case buffer.

      cmd_.end();
      device.resetFences(*fence_);
      ctx_->queue().submit(vk::SubmitInfo {.commandBufferCount = 1, .pCommandBuffers = &*cmd_}, *fence_);
      busy_ = true;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "pyrowave convert() failed: " << e.what();
      return -1;
    }
  }

  encoded_frame pyrowave_encode_device_t::encode_frame(int64_t frame_index) {
    encoded_frame out;
    out.frame_index = frame_index;
    out.idr = true;  // intra-only
    if (!busy_) {
      return out;
    }
    try {
      auto &device = ctx_->device();
      if (device.waitForFences(*fence_, true, UINT64_MAX) != vk::Result::eSuccess) {
        BOOST_LOG(error) << "pyrowave: waitForFences failed";
        return out;
      }
      busy_ = false;

      const void *meta = meta_staging_ ? meta_staging_.map() : meta_buf_.map();
      const auto *blocks = static_cast<const PyroWave::BitstreamPacket *>(meta);
      const int block_count = enc_->block_count_32x32;

      // Used extent of the packed bitstream, from the (tiny) meta table.
      size_t end_words = 0;
      for (int i = 0; i < block_count; ++i) {
        if (blocks[i].num_words) {
          end_words = std::max<size_t>(end_words, size_t(blocks[i].offset_u32) + blocks[i].num_words);
        }
      }

      const void *data;
      if (data_staging_) {
        // Copy only the used region (typically tens of KB) instead of the
        // multi-MB worst-case buffer. Recorded after the encode's fence, so a
        // small second submit is needed on this (non host-visible) path.
        if (end_words) {
          cmd_.reset();
          cmd_.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
          cmd_.copyBuffer(data_buf_, data_staging_, vk::BufferCopy {.size = end_words * sizeof(uint32_t)});
          cmd_.end();
          device.resetFences(*fence_);
          ctx_->queue().submit(vk::SubmitInfo {.commandBufferCount = 1, .pCommandBuffers = &*cmd_}, *fence_);
          if (device.waitForFences(*fence_, true, UINT64_MAX) != vk::Result::eSuccess) {
            BOOST_LOG(error) << "pyrowave: waitForFences (bitstream copy) failed";
            return out;
          }
        }
        data = data_staging_.map();
      } else {
        data = data_buf_.map();
      }

      // Diagnostic (off by default): validate the finished bitstream's per-block
      // self-consistency. It walks every block on the CPU each frame, on the encode
      // critical path, so it is left OFF for latency and enabled on demand with
      // PYROWAVE_VALIDATE=1. Catches a payload_words 12-bit overflow (num_words >=
      // 4096) or any block-layout desync that would turn the rest of the frame to
      // static. Logs the first few frames, then only on a problem or every ~120th.
      {
        static const bool validate_on = [] {
          const char *e = std::getenv("PYROWAVE_VALIDATE");
          return e && std::atoi(e) != 0;
        }();
        if (validate_on) {
          auto vs = enc_->validate_frame(meta, data);
          bool problem = vs.invalid_blocks != 0 || vs.max_num_words >= 4000;
          if (problem || validate_log_count_ < 5 || (validate_log_count_ % 120) == 0) {
            BOOST_LOG(info) << "pyrowave validate[" << validate_log_count_ << "]: invalid="
                            << vs.invalid_blocks << "/" << vs.nonzero_blocks << " nonzero, max_num_words="
                            << vs.max_num_words << " (12-bit cap = 4095)"
                            << (problem ? "  <-- PROBLEM" : "");
          }
          validate_log_count_++;
        }
      }

      // ---- Conditional replenishment + RTP-aligned packetization ----------
      const auto *bitstream_u8 = static_cast<const uint8_t *>(data);

      const bool replenish = refresh_interval_ > 0;
      const bool refresh_requested = full_refresh_.exchange(false, std::memory_order_relaxed);
      const bool full = !replenish || refresh_requested;

      // This frame's 3-bit sequence: read it from the first coded block
      // (fall back to the CPU mirror on a frame with no coded blocks).
      uint32_t seq = seq_mirror_;
      for (int i = 0; i < block_count; ++i) {
        if (blocks[i].num_words) {
          PyroWave::BitstreamHeader hdr;
          std::memcpy(&hdr, bitstream_u8 + size_t(blocks[i].offset_u32) * 4, sizeof(hdr));
          seq = hdr.sequence;
          break;
        }
      }
      if (seq != seq_mirror_ && !logged_seq_mismatch_) {
        logged_seq_mismatch_ = true;
        BOOST_LOG(warning) << "pyrowave: sequence mirror out of sync (mirror=" << seq_mirror_
                           << " bitstream=" << seq << "); using bitstream value";
      }
      seq_mirror_ = seq;

      // Decide per block: 0 = skip, 1 = send coded data, 2 = explicit zero block.
      std::vector<uint8_t> actions(size_t(block_count), 0);
      size_t send_bytes = 0;
      uint32_t sent_blocks = 0;
      for (int i = 0; i < block_count; ++i) {
        const bool present = blocks[i].num_words != 0;
        const uint64_t cur = present
                               ? hash_block_packet(bitstream_u8 + size_t(blocks[i].offset_u32) * 4, size_t(blocks[i].num_words) * 4)
                               : kHashEmpty;
        uint8_t action = 0;
        if (full) {
          // Legacy full frame (code 0): send every coded block; absent blocks
          // decode to zero on the client.
          action = present ? 1 : 0;
        } else {
          // Keep-previous frame (code 1): send only what changed, plus the
          // rolling refresh slice (bounds staleness after packet loss).
          const bool due = (uint64_t(i) % uint64_t(refresh_interval_)) == (frame_counter_ % uint64_t(refresh_interval_));
          if (due || cur != block_hashes_[i]) {
            action = present ? 1 : 2;
          }
        }
        block_hashes_[i] = cur;
        actions[i] = action;
        if (action == 1) {
          send_bytes += size_t(blocks[i].num_words) * 4;
          sent_blocks++;
        } else if (action == 2) {
          send_bytes += sizeof(PyroWave::BitstreamHeader);
          sent_blocks++;
        }
      }

      out.idr = full;
      out.data.clear();
      out.data.reserve(send_bytes + send_bytes / 4 + 4096);

      // RTP alignment: the RTP layer splits the frame into payloads of
      // shard_payload_size_ bytes; the FIRST payload additionally carries the
      // 8-byte video_short_frame_header_t. In-band padding records keep every
      // block packet within a single payload, so a lost payload costs only its
      // own blocks and the parser resyncs at the next payload.
      const bool aligned = shard_payload_size_ != 0;
      const size_t shard = shard_payload_size_;
      size_t next_boundary = aligned ? shard - 8 : SIZE_MAX;

      auto append_bytes = [&](const void *src, size_t len) {
        const auto *b = static_cast<const uint8_t *>(src);
        out.data.insert(out.data.end(), b, b + len);
      };
      auto append_padding = [&](size_t len) {
        // len >= 8 and word-aligned: magic, word count, zero fill.
        const uint32_t rec[2] = {PyroWave::BitstreamPaddingMagic, uint32_t(len / 4 - 2)};
        append_bytes(rec, sizeof(rec));
        out.data.resize(out.data.size() + (len - sizeof(rec)), 0);
      };
      auto append_record = [&](const void *src, size_t len) {
        if (aligned) {
          for (;;) {
            const size_t pos = out.data.size();
            if (next_boundary < pos + 8) {
              // Sliver (< 8 bytes) or already-crossed boundary: cannot hold a
              // padding record; that boundary stays unaligned (rare, only
              // after an oversized record). Move to the next one.
              next_boundary += shard;
              continue;
            }
            const size_t rem = next_boundary - pos;
            if (len > shard) {
              break;  // record larger than a payload: let it span
            }
            if (len <= rem && rem - len != 4) {
              break;  // fits, and doesn't leave an un-paddable 4-byte tail
            }
            append_padding(rem);
            next_boundary += shard;
          }
        }
        append_bytes(src, len);
        if (aligned && out.data.size() == next_boundary) {
          next_boundary += shard;
        }
      };

      // Frame (sequence) header first.
      PyroWave::BitstreamSequenceHeader shdr = {};
      shdr.width_minus_1 = width_ - 1;
      shdr.height_minus_1 = height_ - 1;
      shdr.sequence = seq;
      shdr.extended = 1;
      shdr.code = full ? PyroWave::BITSTREAM_EXTENDED_CODE_START_OF_FRAME
                       : PyroWave::BITSTREAM_EXTENDED_CODE_START_OF_FRAME_KEEP;
      shdr.total_blocks = sent_blocks;
      shdr.chroma_resolution = PyroWave::CHROMA_RESOLUTION_420;
      append_record(&shdr, sizeof(shdr));

      for (int i = 0; i < block_count; ++i) {
        if (actions[i] == 1) {
          append_record(bitstream_u8 + size_t(blocks[i].offset_u32) * 4, size_t(blocks[i].num_words) * 4);
        } else if (actions[i] == 2) {
          // Explicit zero block: header-only packet (ballot 0). The decoder
          // stores zeros, replacing whatever the block previously held.
          PyroWave::BitstreamHeader hdr = {};
          hdr.ballot = 0;
          hdr.payload_words = sizeof(hdr) / sizeof(uint32_t);
          hdr.sequence = seq;
          hdr.extended = 0;
          hdr.quant_code = 0;
          hdr.block_index = uint32_t(i);
          append_record(&hdr, sizeof(hdr));
        }
      }

      frame_counter_++;

      // Adaptive budget: spend replenishment savings on quality. Coarse, rare
      // steps keep the RDO's quant decisions stable frame-to-frame (stability
      // is what makes block skipping effective); an over-budget frame resets
      // the scale immediately.
      if (replenish) {
        const size_t wire = out.data.size();
        if (wire > target_size_) {
          if (budget_scale_ != 1.0) {
            BOOST_LOG(debug) << "pyrowave: resetting RDO budget scale (frame used " << wire << " bytes)";
          }
          budget_scale_ = 1.0;
          low_usage_frames_ = 0;
        } else if (wire * 2 < target_size_ && budget_scale_ < 2.0) {
          if (++low_usage_frames_ >= 120) {
            budget_scale_ = std::min(2.0, budget_scale_ * 1.5);
            low_usage_frames_ = 0;
            BOOST_LOG(info) << "pyrowave: raising RDO budget scale to " << budget_scale_
                            << " (spending replenishment savings on quality)";
          }
        } else {
          low_usage_frames_ = 0;
        }
      }

      if (!logged_first_encode_) {
        logged_first_encode_ = true;
        BOOST_LOG(info) << "pyrowave: first frame encoded " << out.data.size() << " bytes ("
                        << sent_blocks << "/" << block_count << " blocks, "
                        << (full ? "full" : "keep") << ")";
      }
      return out;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "pyrowave encode_frame() failed: " << e.what();
      out.data.clear();
      return out;
    }
  }

  size_t pyrowave_encode_device_t::adaptive_target_size() const {
    return size_t(double(target_size_) * budget_scale_);
  }

  pyrowave_encode_device_t::~pyrowave_encode_device_t() {
    try {
      if (busy_ && *fence_) {
        (void) ctx_->device().waitForFences(*fence_, true, UINT64_MAX);
      }
    } catch (...) {
    }
  }

}  // namespace pyrowave_enc
