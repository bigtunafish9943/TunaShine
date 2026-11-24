<template>
  <!-- Send a file from this PC to a paired device; TunaReceive collects it while it's open -->
  <div class="card p-2 my-4" v-if="devices.length">
    <div class="card-body">
      <h2>Send to a device</h2>
      <p>
        Pick a file and a device. TunaReceive saves it in the device's Downloads the next time it's open, and offers
        to install it if it's an app (an APK). Up to {{ maxSizeText }} a file.
      </p>

      <div class="send-file-row">
        <select class="form-select" v-model="target" aria-label="Device">
          <option v-for="d in devices" :key="d.uuid" :value="d.uuid">{{ d.name }}</option>
        </select>
        <input class="form-control" type="file" ref="file" @change="picked = $refs.file.files.length > 0" aria-label="File">
        <button class="btn btn-primary" :disabled="!picked || !target || sending" @click="send">
          <span v-if="sending" class="spinner-border spinner-border-sm" aria-hidden="true"></span>
          <i v-else class="fas fa-paper-plane"></i> Send
        </button>
      </div>

      <div class="alert alert-danger mt-3" v-if="error">{{ error }}</div>
      <div class="alert alert-success mt-3" v-if="sent">
        {{ sent }} is waiting for {{ nameOf(target) }}. Open TunaReceive on it to receive it.
      </div>

      <div v-if="waiting.length" class="mt-3">
        <h3 class="h6">Waiting to be collected</h3>
        <ul class="list-unstyled send-file-list">
          <li v-for="f in waiting" :key="f.uuid + f.id">
            <span><strong>{{ f.name }}</strong> for {{ nameOf(f.uuid) }} ({{ sizeText(f.size) }})</span>
            <button class="btn btn-sm btn-outline-secondary" @click="cancel(f)">Don't send</button>
          </li>
        </ul>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  data() {
    return {
      devices: [],
      target: '',
      picked: false,
      sending: false,
      sent: '',
      error: '',
      waiting: [],
      maxSize: 256 * 1024 * 1024,
      timer: null,
    };
  },
  computed: {
    maxSizeText() {
      return this.sizeText(this.maxSize);
    },
  },
  async created() {
    try {
      const clients = await fetch('./api/clients/list', { credentials: 'include' }).then((r) => r.json());
      this.devices = (clients.named_certs || []).map((c) => ({ uuid: c.uuid, name: c.name }));
      if (this.devices.length) {
        this.target = this.devices[0].uuid;
      }
    } catch (e) {
      this.devices = [];
    }
    this.refresh();
    // Files disappear from the list as devices collect them
    this.timer = setInterval(() => this.refresh(), 5000);
  },
  beforeUnmount() {
    clearInterval(this.timer);
  },
  methods: {
    nameOf(uuid) {
      const d = this.devices.find((x) => x.uuid === uuid);
      return d ? d.name : 'a device';
    },
    sizeText(bytes) {
      if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(bytes >= 10 * 1024 * 1024 ? 0 : 1) + ' MB';
      if (bytes >= 1024) return Math.round(bytes / 1024) + ' KB';
      return bytes + ' bytes';
    },
    async refresh() {
      try {
        const r = await fetch('./api/files', { credentials: 'include' }).then((x) => x.json());
        if (r.maxSize) this.maxSize = r.maxSize;
        const list = [];
        for (const [uuid, files] of Object.entries(r.devices || {})) {
          for (const f of files) list.push({ uuid, ...f });
        }
        this.waiting = list;
      } catch (e) {
        // Leave the list as it was
      }
    },
    async send() {
      const file = this.$refs.file.files[0];
      if (!file) return;
      this.error = '';
      this.sent = '';
      if (file.size > this.maxSize) {
        this.error = `${file.name} is ${this.sizeText(file.size)}; files can be up to ${this.maxSizeText}.`;
        return;
      }
      this.sending = true;
      try {
        const url = `./api/files/send?uuid=${encodeURIComponent(this.target)}&name=${encodeURIComponent(file.name)}`;
        const r = await fetch(url, {
          method: 'POST',
          credentials: 'include',
          headers: { 'Content-Type': 'application/octet-stream' },
          body: file,
        }).then((x) => x.json());
        if (r.status) {
          this.sent = file.name;
          this.$refs.file.value = '';
          this.picked = false;
          this.refresh();
        } else {
          this.error = r.error || 'The file wasn\'t sent.';
        }
      } catch (e) {
        this.error = 'The file wasn\'t sent. Check that TunaStream is still running.';
      } finally {
        this.sending = false;
      }
    },
    async cancel(f) {
      await fetch('./api/files/cancel', {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ uuid: f.uuid, id: f.id }),
      });
      this.refresh();
    },
  },
};
</script>

<style scoped>
.send-file-row {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  align-items: center;
}

.send-file-row select {
  flex: 0 1 14rem;
}

.send-file-row input {
  flex: 1 1 16rem;
}

.send-file-list li {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 1rem;
  padding: 0.4rem 0;
  border-bottom: 1px solid var(--bs-border-color);
}
</style>
