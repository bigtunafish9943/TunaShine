<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
</script>

<template>
  <div id="input" class="config-page">
    <!-- Controllers -->
    <Checkbox class="mb-3"
              id="controller"
              locale-prefix="config"
              v-model="config.controller"
              default="true"
    ></Checkbox>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #linux>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>

          <template #windows>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
    </div>

    <!-- Rumble -->
    <Checkbox v-if="config.controller === 'enabled' && platform === 'windows'" class="mb-3"
              id="forward_rumble"
              locale-prefix="config"
              v-model="config.forward_rumble"
              default="true"
    ></Checkbox>

    <!-- Keyboard and mouse -->
    <Checkbox class="mb-3"
              id="keyboard"
              locale-prefix="config"
              v-model="config.keyboard"
              default="true"
    ></Checkbox>
    <Checkbox class="mb-3"
              id="mouse"
              locale-prefix="config"
              v-model="config.mouse"
              default="true"
    ></Checkbox>

    <!-- Input Only Mode -->
    <Checkbox class="mb-3"
              id="enable_input_only_mode"
              locale-prefix="config"
              v-model="config.enable_input_only_mode"
              default="false"
    ></Checkbox>

    <details class="app-more mt-4">
      <summary>{{ $t('config.advanced_title') }}</summary>
      <p class="form-text">{{ $t('config.input_advanced_desc') }}</p>

      <!-- Controller details -->
      <template v-if="config.controller === 'enabled'">
        <h3 class="h6 mt-3">{{ $t('config.input_controller_more') }}</h3>
        <template v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')">
          <Checkbox class="mb-3"
                    id="motion_as_ds4"
                    locale-prefix="config"
                    v-model="config.motion_as_ds4"
                    default="false"
          ></Checkbox>
          <Checkbox class="mb-3"
                    id="touchpad_as_ds4"
                    locale-prefix="config"
                    v-model="config.touchpad_as_ds4"
                    default="true"
          ></Checkbox>
        </template>
        <template v-if="config.gamepad === 'ds4' || (config.gamepad === 'auto' && platform === 'windows')">
          <Checkbox class="mb-3"
                    id="ds4_back_as_touchpad_click"
                    locale-prefix="config"
                    v-model="config.ds4_back_as_touchpad_click"
                    default="true"
          ></Checkbox>
        </template>
        <template v-if="config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform === 'linux')">
          <Checkbox class="mb-3"
                    id="ds5_inputtino_randomize_mac"
                    locale-prefix="config"
                    v-model="config.ds5_inputtino_randomize_mac"
                    default="true"
          ></Checkbox>
        </template>
        <div class="mb-3">
          <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
          <input type="text" class="form-control" id="back_button_timeout" placeholder="-1"
                 v-model="config.back_button_timeout" />
          <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
        </div>
      </template>

      <!-- Keyboard details -->
      <template v-if="config.keyboard === 'enabled'">
        <h3 class="h6 mt-3">{{ $t('config.input_keyboard_more') }}</h3>
        <div class="mb-3" v-if="platform === 'windows'">
          <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
          <input type="text" class="form-control" id="key_repeat_delay" placeholder="500"
                 v-model="config.key_repeat_delay" />
          <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
        </div>
        <div class="mb-3" v-if="platform === 'windows'">
          <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
          <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9"
                 v-model="config.key_repeat_frequency" />
          <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
        </div>
        <Checkbox v-if="platform === 'windows'" class="mb-3"
                  id="always_send_scancodes"
                  locale-prefix="config"
                  v-model="config.always_send_scancodes"
                  default="true"
        ></Checkbox>
        <Checkbox class="mb-3"
                  id="key_rightalt_to_key_win"
                  locale-prefix="config"
                  v-model="config.key_rightalt_to_key_win"
                  default="false"
        ></Checkbox>
      </template>

      <!-- Mouse details -->
      <template v-if="config.mouse === 'enabled'">
        <h3 class="h6 mt-3">{{ $t('config.input_mouse_more') }}</h3>
        <Checkbox class="mb-3"
                  id="high_resolution_scrolling"
                  locale-prefix="config"
                  v-model="config.high_resolution_scrolling"
                  default="true"
        ></Checkbox>
        <Checkbox class="mb-3"
                  id="native_pen_touch"
                  locale-prefix="config"
                  v-model="config.native_pen_touch"
                  default="true"
        ></Checkbox>
      </template>
    </details>
  </div>
</template>

<style scoped>

</style>
