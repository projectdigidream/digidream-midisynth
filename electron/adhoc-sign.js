// electron-builder afterPack: ad-hoc sign the macOS app, before its dmg and zip
// are made. Without a Developer ID there is no real signature to give it, and
// electron-builder (24) then leaves Electron's own signature broken by the
// renamed bundle -- which Apple silicon refuses to run at all ("code has no
// resources but signature indicates they must be present"). An ad-hoc
// signature is valid; Gatekeeper still asks before a downloaded copy opens.
const { execFileSync } = require('node:child_process')
const path = require('node:path')

exports.default = async function (ctx) {
  if (ctx.electronPlatformName !== 'darwin') return
  const app = path.join(ctx.appOutDir, `${ctx.packager.appInfo.productFilename}.app`)
  execFileSync('codesign', ['--force', '--deep', '--sign', '-', app], { stdio: 'inherit' })
  execFileSync('codesign', ['--verify', '--deep', '--strict', app], { stdio: 'inherit' })
}
