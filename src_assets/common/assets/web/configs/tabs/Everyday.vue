<script setup>
import { ref } from 'vue'
import Checkbox from '../../Checkbox.vue'

const props = defineProps(['platform', 'config'])
const config = ref(props.config)

const on = (value) => value !== 'disabled' && value !== false
</script>

<template>
  <div id="everyday" class="config-page everyday">
    <p class="mb-4">{{ $t('config.everyday_intro') }}</p>

    <section class="everyday-group" aria-labelledby="everyday-screen">
      <h2 class="h5" id="everyday-screen">{{ $t('config.everyday_screen') }}</h2>
      <Checkbox class="mb-3" id="headless_mode" locale-prefix="config" v-model="config.headless_mode" default="true"
                v-if="platform === 'windows'"></Checkbox>
      <Checkbox class="mb-3" id="shared_pc" locale-prefix="config" v-model="config.shared_pc" default="true"></Checkbox>
      <Checkbox class="mb-3 ms-4" id="shared_pc_borderless" locale-prefix="config" v-model="config.shared_pc_borderless" default="true"
                v-if="on(config.shared_pc)"></Checkbox>
      <Checkbox class="mb-3 ms-4" id="privacy_mode" locale-prefix="config" v-model="config.privacy_mode" default="false"
                v-if="!on(config.shared_pc)"></Checkbox>
      <Checkbox class="mb-3" id="quiet_notifications" locale-prefix="config" v-model="config.quiet_notifications" default="true"
                v-if="platform === 'windows'"></Checkbox>
    </section>

    <section class="everyday-group" aria-labelledby="everyday-smooth">
      <h2 class="h5" id="everyday-smooth">{{ $t('config.everyday_smooth') }}</h2>
      <Checkbox class="mb-3" id="game_frame_limit" locale-prefix="config" v-model="config.game_frame_limit" default="true"
                v-if="platform === 'windows'"></Checkbox>
      <Checkbox class="mb-3 ms-4" id="game_vsync_off" locale-prefix="config" v-model="config.game_vsync_off" default="true"
                v-if="platform === 'windows' && on(config.game_frame_limit)"></Checkbox>
      <Checkbox class="mb-3" id="adaptive_fec" locale-prefix="config" v-model="config.adaptive_fec" default="true"></Checkbox>
      <Checkbox class="mb-3" id="double_refreshrate" locale-prefix="config" v-model="config.double_refreshrate" default="false"
                v-if="platform === 'windows'"></Checkbox>
      <Checkbox class="mb-3" id="end_session_on_disconnect" locale-prefix="config" v-model="config.end_session_on_disconnect" default="false"></Checkbox>
    </section>

    <section class="everyday-group" aria-labelledby="everyday-games">
      <h2 class="h5" id="everyday-games">{{ $t('config.everyday_games') }}</h2>
      <ul class="everyday-links">
        <li><a href="./apps"><i class="fas fa-fw fa-gamepad" aria-hidden="true"></i> {{ $t('config.everyday_apps') }}</a></li>
        <li><a href="./pin"><i class="fas fa-fw fa-tv" aria-hidden="true"></i> {{ $t('config.everyday_devices') }}</a></li>
        <li><a href="./watch"><i class="fas fa-fw fa-eye" aria-hidden="true"></i> {{ $t('config.everyday_watch') }}</a></li>
      </ul>
    </section>

    <section class="everyday-group" aria-labelledby="everyday-fix">
      <h2 class="h5" id="everyday-fix">{{ $t('config.everyday_fix') }}</h2>
      <ul class="everyday-links">
        <li><a href="./troubleshooting#vdisplay"><i class="fas fa-fw fa-display" aria-hidden="true"></i> {{ $t('config.everyday_repair') }}</a></li>
        <li><a href="./troubleshooting#restart"><i class="fas fa-fw fa-rotate" aria-hidden="true"></i> {{ $t('config.everyday_restart') }}</a></li>
        <li><a href="./troubleshooting#tv_logs"><i class="fas fa-fw fa-file-lines" aria-hidden="true"></i> {{ $t('config.everyday_logs') }}</a></li>
        <li><a href="./checklist"><i class="fas fa-fw fa-list-check" aria-hidden="true"></i> {{ $t('config.everyday_checklist') }}</a></li>
      </ul>
    </section>
  </div>
</template>

<style scoped>
.everyday-group {
  padding-bottom: 1rem;
  margin-bottom: 1.5rem;
  border-bottom: 1px solid var(--bs-border-color);
}

.everyday-group:last-child {
  border-bottom: 0;
}

.everyday-links {
  list-style: none;
  padding: 0;
  margin: 0;
  display: grid;
  gap: 0.5rem;
}
</style>
