<script setup>
import { ref } from 'vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="pyrowave-encoder" class="config-page">
    <!-- Explainer -->
    <div class="alert alert-info">
      {{ $t('config.pyrowave_tab_desc') }}
    </div>

    <!-- Coefficient storage precision -->
    <div class="mb-3">
      <label for="pyrowave_precision" class="form-label">{{ $t('config.pyrowave_precision') }}</label>
      <select id="pyrowave_precision" class="form-select" v-model="config.pyrowave_precision">
        <option value="0">{{ $t('config.pyrowave_precision_0') }}</option>
        <option value="1">{{ $t('config.pyrowave_precision_1') }}</option>
        <option value="2">{{ $t('config.pyrowave_precision_2') }}</option>
      </select>
      <div class="form-text">{{ $t('config.pyrowave_precision_desc') }}</div>
    </div>

    <!-- Quality bias -->
    <div class="mb-3">
      <label for="pyrowave_quality_bias" class="form-label">{{ $t('config.pyrowave_quality_bias') }}</label>
      <select id="pyrowave_quality_bias" class="form-select" v-model="config.pyrowave_quality_bias">
        <option value="0">{{ $t('config.pyrowave_quality_bias_0') }}</option>
        <option value="1">+1</option>
        <option value="2">+2</option>
        <option value="3">+3</option>
      </select>
      <div class="form-text">{{ $t('config.pyrowave_quality_bias_desc') }}</div>
    </div>

    <!-- Refresh interval (conditional replenishment) -->
    <div class="mb-3">
      <label for="pyrowave_refresh_interval" class="form-label">{{ $t('config.pyrowave_refresh_interval') }}</label>
      <input type="number" class="form-control" id="pyrowave_refresh_interval"
             placeholder="8" min="0" max="255" v-model="config.pyrowave_refresh_interval" />
      <div class="form-text">{{ $t('config.pyrowave_refresh_interval_desc') }}</div>
    </div>

    <!-- Cursor compositing -->
    <Checkbox class="mb-3"
              id="pyrowave_capture_cursor"
              locale-prefix="config"
              v-model="config.pyrowave_capture_cursor"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
