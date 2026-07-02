<script setup>
import { ref, watch } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config',
  'global_prep_cmd'
])

const config = ref(props.config)

// Selecting PyroWave as the encoder takes over the old "Force PyroWave codec"
// checkbox: advertise only PyroWave to clients (it cannot encode H.264/HEVC/
// AV1, so leaving those advertised would break negotiation) and reset the
// settings below that don't apply to it. Codec-specific tuning lives in the
// PyroWave tab.
watch(() => config.value.encoder, (encoder, oldEncoder) => {
  if (encoder === 'pyrowave') {
    config.value.force_pyrowave = true
    // PyroWave defaults: low FEC (loss heals via keep-previous + refresh).
    config.value.fec_percentage = 10
    // Not applicable to PyroWave; reset so stale values don't linger.
    config.value.qp = 28
    config.value.min_threads = 2
    config.value.hevc_mode = 0
    config.value.av1_mode = 0
  }
  else if (oldEncoder === 'pyrowave') {
    config.value.force_pyrowave = false
    // Restore the stock FEC default for the traditional codecs.
    config.value.fec_percentage = 20
  }
})
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-3">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input type="text" class="form-control" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter (not used by PyroWave) -->
    <div class="mb-3" v-if="config.encoder !== 'pyrowave'">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <input type="number" class="form-control" id="qp" placeholder="28" v-model="config.qp" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads (software encoder only; not used by PyroWave) -->
    <div class="mb-3" v-if="config.encoder !== 'pyrowave'">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input type="number" class="form-control" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support (PyroWave advertises only its own codec) -->
    <div class="mb-3" v-if="config.encoder !== 'pyrowave'">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <select id="hevc_mode" class="form-select" v-model="config.hevc_mode">
        <option value="0">{{ $t('config.hevc_mode_0') }}</option>
        <option value="1">{{ $t('config.hevc_mode_1') }}</option>
        <option value="2">{{ $t('config.hevc_mode_2') }}</option>
        <option value="3">{{ $t('config.hevc_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support (PyroWave advertises only its own codec) -->
    <div class="mb-3" v-if="config.encoder !== 'pyrowave'">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <select id="av1_mode" class="form-select" v-model="config.av1_mode">
        <option value="0">{{ $t('config.av1_mode_0') }}</option>
        <option value="1">{{ $t('config.av1_mode_1') }}</option>
        <option value="2">{{ $t('config.av1_mode_2') }}</option>
        <option value="3">{{ $t('config.av1_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- Capture -->
    <div class="mb-3" v-if="platform !== 'macos'">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <select id="capture" class="form-select" v-model="config.capture">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="wlr">wlroots</option>
            <option value="x11">X11</option>
            <option value="portal">XDG Portal</option>
          </template>
          <template #linux>
            <option value="nvfbc">NvFBC</option>
            <option value="wlr">wlroots</option>
            <option value="kms">KMS</option>
            <option value="x11">X11</option>
            <option value="kwin">KWin Screencast</option>
            <option value="portal">XDG Portal</option>
          </template>
          <template #windows>
            <option value="ddx">Desktop Duplication API</option>
            <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.capture_desc') }}</div>
    </div>

    <!-- Encoder -->
    <div class="mb-3">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" class="form-select" v-model="config.encoder">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #freebsd>
            <option value="vulkan">Vulkan</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
            <option value="vulkan">Vulkan</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
        <option value="pyrowave">PyroWave (GPU wavelet)</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
      <div class="form-text" v-if="config.encoder === 'pyrowave'">{{ $t('config.encoder_pyrowave_note') }}</div>
    </div>

  </div>
</template>

<style scoped>

</style>
