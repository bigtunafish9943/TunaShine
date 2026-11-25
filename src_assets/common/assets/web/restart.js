/**
 * TunaStream restarts to apply settings, and its page can't be reached for a few seconds.
 * Rather than reloading into "This site can't be reached", wait until it has gone down and
 * come back, then reload. Resolves false if it hasn't come back after a minute and a half.
 */
export async function reloadWhenBack() {
  const answers = async () => {
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), 1500)
    try {
      await fetch('./api/configLocale', { cache: 'no-store', signal: controller.signal })
      return true
    } catch (e) {
      return false
    } finally {
      clearTimeout(timer)
    }
  }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
  const started = Date.now()
  // First it goes away (or a few seconds pass, if the restart was very quick)...
  while (Date.now() - started < 8000 && await answers()) {
    await sleep(500)
  }
  // ...then it comes back
  while (Date.now() - started < 90000) {
    if (await answers()) {
      location.reload()
      return true
    }
    await sleep(1000)
  }
  return false
}
