<script setup>
import { ref } from 'vue'
import Checkbox from '../../Checkbox.vue'

const props = defineProps(['platform', 'config'])
const config = ref(props.config)
</script>

<template>
  <div id="sharedpc" class="config-page">
    <p class="mb-4">{{ $t('config.sharedpc_intro') }} <a href="./help#shared-pc">{{ $t('config.sharedpc_more') }}</a></p>

    <!-- Shared PC mode -->
    <Checkbox class="mb-3"
              id="shared_pc"
              locale-prefix="config"
              v-model="config.shared_pc"
              default="true"
    ></Checkbox>

    <!-- Shared PC: which screen is the main one -->
    <Checkbox class="mb-3"
              id="shared_pc_tv_primary"
              locale-prefix="config"
              v-model="config.shared_pc_tv_primary"
              default="false"
              v-if="config.shared_pc !== 'disabled'"
    ></Checkbox>

    <!-- Shared PC: windowed games fill the TV -->
    <Checkbox class="mb-3"
              id="shared_pc_borderless"
              locale-prefix="config"
              v-model="config.shared_pc_borderless"
              default="true"
              v-if="config.shared_pc !== 'disabled'"
    ></Checkbox>
  </div>
</template>
