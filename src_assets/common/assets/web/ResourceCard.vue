<template>
    <div class="card">
        <div class="card-body">
            <h2>{{ $t('resource_card.resources') }}</h2>
            <p>{{ $t('resource_card.resources_desc') }}</p>
            <div class="d-flex flex-wrap gap-2 mt-4">
                <a class="btn btn-solarflare" href="https://github.com/Koloses/Solarflare" target="_blank">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  Solarflare
                </a>
                <a class="btn btn-solarflare" href="https://github.com/Koloses/aurora-qt" target="_blank">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  Aurora (PC)
                </a>
                <a class="btn btn-solarflare" href="https://github.com/Koloses/aurora-android" target="_blank">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  Aurora (Android)
                </a>
            </div>
        </div>
    </div>
    <!-- Legal -->
    <div class="card mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.legal') }}</h2>
            <p>{{ $t('resource_card.legal_desc') }}</p>
            <div class="d-flex flex-wrap gap-2 mt-4">
                <a class="btn btn-danger" href="https://github.com/Koloses/Solarflare/blob/master/LICENSE"
                    target="_blank">
                  <FileText :size="18" class="icon"></FileText>
                  {{ $t('resource_card.license') }}
                </a>
                <a class="btn btn-danger" href="https://github.com/Koloses/Solarflare/blob/master/NOTICE"
                    target="_blank">
                  <AlertCircle :size="18" class="icon"></AlertCircle>
                  {{ $t('resource_card.third_party_notice') }}
                </a>
            </div>
        </div>
    </div>
</template>

<script>
import {
  AlertCircle,
  FileText,
} from '@lucide/vue'
import SimpleIcon from './SimpleIcon.vue'

export default {
  components: {
    SimpleIcon,
    AlertCircle,
    FileText,
  },
}
</script>
