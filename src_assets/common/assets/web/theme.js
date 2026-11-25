const getStoredTheme = () => localStorage.getItem('theme')
const setStoredTheme = theme => localStorage.setItem('theme', theme)

export const getPreferredTheme = () => {
    const storedTheme = getStoredTheme()
    if (storedTheme) {
        return storedTheme
    }

    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

const setTheme = theme => {
    if (theme === 'auto') {
        document.documentElement.setAttribute(
            'data-bs-theme',
            (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')
        )
    } else {
        document.documentElement.setAttribute('data-bs-theme', theme)
    }
}

export const showActiveTheme = (theme, focus = false) => {
    const themeSwitcher = document.querySelector('#bd-theme')

    if (!themeSwitcher) {
        return
    }

    const themeSwitcherText = document.querySelector('#bd-theme-text')
    const activeThemeIcon = document.querySelector('.theme-icon-active i')
    const btnToActive = document.querySelector(`[data-bs-theme-value="${theme}"]`)
    const classListOfActiveBtn = btnToActive.querySelector('i').classList

    document.querySelectorAll('[data-bs-theme-value]').forEach(element => {
        element.classList.remove('active')
        element.setAttribute('aria-pressed', 'false')
    })

    btnToActive.classList.add('active')
    btnToActive.setAttribute('aria-pressed', 'true')
    activeThemeIcon.classList.remove(...activeThemeIcon.classList.values())
    activeThemeIcon.classList.add(...classListOfActiveBtn)
    const themeSwitcherLabel = `${themeSwitcherText.textContent} (${btnToActive.textContent.trim()})`
    themeSwitcher.setAttribute('aria-label', themeSwitcherLabel)

    if (focus) {
        themeSwitcher.focus()
    }
}

export function setupThemeToggleListener() {
    document.querySelectorAll('[data-bs-theme-value]')
        .forEach(toggle => {
            toggle.addEventListener('click', () => {
                const theme = toggle.getAttribute('data-bs-theme-value')
                setStoredTheme(theme)
                setTheme(theme)
                showActiveTheme(theme, true)
            })
        })

    showActiveTheme(getPreferredTheme(), false)
}

const COLOUR_THEMES = ['deepsea', 'reef', 'kelp', 'midnight', 'gold']

/**
 * Wears a colour theme (Deep Sea, Coral Reef, ...). The choice is saved on the PC; it is also
 * remembered in this browser so the next page opens in it straight away, before the PC answers.
 */
export function applyColourTheme(theme) {
    if (!COLOUR_THEMES.includes(theme)) {
        return
    }
    document.documentElement.setAttribute('data-ts-theme', theme)
    try {
        localStorage.setItem('tunastream.colourTheme', theme)
    } catch (e) {
        // Storage can be refused; the PC's answer still applies it
    }
}

function applyRememberedColourTheme() {
    try {
        const theme = localStorage.getItem('tunastream.colourTheme')
        if (COLOUR_THEMES.includes(theme)) {
            document.documentElement.setAttribute('data-ts-theme', theme)
        }
    } catch (e) {
        // No storage: the default until the PC answers
    }
}

export function loadAutoTheme() {
    applyRememberedColourTheme()
    setTheme(getPreferredTheme())

    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
        const storedTheme = getStoredTheme()
        if (storedTheme !== 'light' && storedTheme !== 'dark') {
            setTheme(getPreferredTheme())
        }
    })

    window.addEventListener('DOMContentLoaded', () => {
        showActiveTheme(getPreferredTheme())
    })
}
