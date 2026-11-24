<template>
  <!-- Quiet footer: help, reporting a problem, and the license notices the GPL asks for -->
  <footer class="page-footer">
    <nav class="page-footer-links" :aria-label="$t('resource_card.resources')">
      <a href="./help"><i class="fas fa-fw fa-life-ring"></i> {{ $t('resource_card.troubleshooting_guide') }}</a>
      <a href="https://github.com/bigtunafish9943/TunaStream/issues" target="_blank" rel="noopener"><i class="fab fa-fw fa-github"></i> {{ $t('resource_card.github_discussions') }}</a>
      <a href="https://github.com/bigtunafish9943/TunaStream/blob/master/LICENSE" target="_blank" rel="noopener">{{ $t('resource_card.license') }}</a>
      <a href="https://github.com/bigtunafish9943/TunaStream/blob/master/NOTICE" target="_blank" rel="noopener">{{ $t('resource_card.third_party_notice') }}</a>
    </nav>
    <p class="page-footer-credit">{{ $t('resource_card.credit') }}</p>
  </footer>
</template>
