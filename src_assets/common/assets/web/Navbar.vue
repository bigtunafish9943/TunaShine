<template>
  <nav ref="nav" class="navbar navbar-light navbar-expand-lg navbar-background header">
    <div class="container">
      <a class="navbar-brand" href="./" title="TunaStream">
        <img src="/images/logo-tunastream-45.png" height="34" alt="">
        <span class="brand-stack">
          <span class="brand-word">Tuna<b>Stream</b></span>
          <span class="brand-version" v-if="installedVersion">{{ $t('navbar.version', { version: installedVersion }) }}</span>
        </span>
      </a>
      <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navbarSupportedContent"
              aria-controls="navbarSupportedContent" aria-expanded="false" aria-label="Toggle navigation">
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav me-auto mb-2 mb-lg-0">
          <li class="nav-item">
            <a class="nav-link" href="./"><i class="fas fa-fw fa-home"></i> {{ $t('navbar.home') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./pin"><i class="fas fa-fw fa-mobile-screen"></i> {{ $t('navbar.pin') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./apps"><i class="fas fa-fw fa-stream"></i> {{ $t('navbar.applications') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./config"><i class="fas fa-fw fa-cog"></i> {{ $t('navbar.configuration') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./password"><i class="fas fa-fw fa-user-shield"></i> {{ $t('navbar.password') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./troubleshooting"><i class="fas fa-fw fa-info"></i> {{ $t('navbar.troubleshoot') }}</a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="./help"><i class="fas fa-fw fa-life-ring"></i> {{ $t('navbar.help') }}</a>
          </li>
          <li class="nav-item nav-item-theme">
            <ThemeToggle/>
          </li>
        </ul>
      </div>
    </div>
  </nav>

  <!-- Shown on every page when a newer release is out, until closed for that version -->
  <div class="update-banner" v-if="updateVersion && !dismissed" role="status">
    <i class="fas fa-circle-arrow-up" aria-hidden="true"></i>
    <span class="update-banner-text">{{ $t('navbar.update_banner', { version: updateVersion }) }}</span>
    <a class="btn btn-sm btn-primary" href="./#update">{{ $t('navbar.update_banner_action') }}</a>
    <button type="button" class="update-banner-close" @click="dismiss" :aria-label="$t('navbar.update_banner_close')" :title="$t('navbar.update_banner_close')">
      <i class="fas fa-xmark" aria-hidden="true"></i>
    </button>
  </div>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'
import TunaStreamVersion from './tunastream_version'

const DISMISS_KEY = 'tunastream.dismissedUpdate';

export default {
  components: { ThemeToggle },
  data() {
    return {
      updateVersion: '',
      installedVersion: '',
      dismissed: false,
    };
  },
  async created() {
    try {
      const config = await fetch("./api/config", { credentials: 'include' }).then((r) => r.ok ? r.json() : null);
      if (!config || !config.version) return;
      this.installedVersion = config.version;
      const latest = await fetch("https://api.github.com/repos/bigtunafish9943/TunaStream/releases/latest").then((r) => r.ok ? r.json() : null);
      if (!latest) return;
      const installed = new TunaStreamVersion(null, config.version);
      const available = new TunaStreamVersion(latest, null);
      if (available.isGreater(installed)) {
        this.updateVersion = available.version;
        try {
          this.dismissed = localStorage.getItem(DISMISS_KEY) === available.version;
        } catch (e) {
          this.dismissed = false;
        }
      }
    } catch (e) {
      // No banner when GitHub can't be reached
    }
  },
  methods: {
    dismiss() {
      this.dismissed = true;
      try {
        // Remembered per version, so the next release shows the banner again
        localStorage.setItem(DISMISS_KEY, this.updateVersion);
      } catch (e) {
        // Closing still works for this page view
      }
    },
  },
  mounted() {
    // Mark the link for the current page. Links are relative ("./pin") while the path is
    // absolute ("/pin"), so compare the last path segment.
    const page = document.location.pathname.replace(/\/+$/, "").split("/").pop().replace(/\.html$/, "");
    const target = (page === "" || page === "index") ? "./" : "./" + page;
    const el = this.$refs.nav.querySelector("a.nav-link[href='" + target + "']");
    if (el) el.classList.add("active")
  }
}
</script>

<style>
/* Navbar colors live in assets/css/tunastream.css with the rest of the theme */
.form-control::placeholder {
  opacity: 0.5;
}
</style>
