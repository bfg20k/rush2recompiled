// Compiles and links every shader from dump_glsl in WebGL2 (Chromium + SwiftShader). Usage: node validate_glsl.js shaders.jsonl
const { chromium } = require('playwright');
const fs = require('fs');

(async () => {
  const lines = fs.readFileSync(process.argv[2], 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
  const browser = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage();
  const results = await page.evaluate((shaders) => {
    const gl = document.createElement('canvas').getContext('webgl2');
    if (!gl) return { error: 'no webgl2' };
    const bad = [];
    for (const s of shaders) {
      const prog = gl.createProgram();
      let log = '';
      for (const [type, src, name] of [[gl.VERTEX_SHADER, s.vs, 'vs'], [gl.FRAGMENT_SHADER, s.fs, 'fs']]) {
        const sh = gl.createShader(type);
        gl.shaderSource(sh, src);
        gl.compileShader(sh);
        if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) log += name + ': ' + gl.getShaderInfoLog(sh) + '\n';
        gl.attachShader(prog, sh);
      }
      if (!log) {
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) log += 'link: ' + gl.getProgramInfoLog(prog);
      }
      if (log) bad.push({ id: s.id, log, vs: s.vs, fs: s.fs });
      gl.deleteProgram(prog);
    }
    return { total: shaders.length, bad: bad.slice(0, 5), badCount: bad.length, renderer: gl.getParameter(gl.VERSION) };
  }, lines);
  console.log(JSON.stringify(results, null, 1));
  await browser.close();
  process.exit(results.error || results.badCount ? 1 : 0);
})();
