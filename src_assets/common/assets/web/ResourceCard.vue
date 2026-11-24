<template>
    <div class="card p-2 mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.resources') }}</h2>
            <br>
            <p>{{ $t('resource_card.resources_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a class="btn btn-secondary m-1" href="https://github.com/bigtunafish9943/TunaShine/blob/master/docs/troubleshooting.md" target="_blank">
                    <i class="fas fa-fw fa-life-ring"></i> {{ $t('resource_card.troubleshooting_guide') }}</a>
                <a class="btn btn-secondary m-1" href="https://github.com/bigtunafish9943/TunaShine/issues" target="_blank">
                    <i class="fab fa-fw fa-github"></i> {{ $t('resource_card.github_discussions') }}</a>
            </div>
        </div>
    </div>
    <!-- Legal -->
    <div class="card p-2 mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.legal') }}</h2>
            <br>
            <p>{{ $t('resource_card.legal_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a class="btn btn-danger m-1" href="https://github.com/bigtunafish9943/TunaShine/blob/master/LICENSE"
                    target="_blank">
                    <i class="fas fa-fw fa-file-alt"></i> {{ $t('resource_card.license') }}</a>
                <a class="btn btn-danger m-1" href="https://github.com/bigtunafish9943/TunaShine/blob/master/NOTICE"
                    target="_blank">
                    <i class="fas fa-fw fa-exclamation"></i> {{ $t('resource_card.third_party_notice') }}</a>
            </div>
        </div>
    </div>
</template>
