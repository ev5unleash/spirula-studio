// WebGL2 viewport for the IMU/GPS tool: basemap tiles on the ground plane,
// GPS tracks in local metres with altitude, and the IMU drawn where it was
// measured (an attitude triad and the sensor vectors at the playhead).
//
// Polylines are instanced screen-space quads — one instance per segment
// reading p0/p1 out of the same position buffer at two offsets — so a track
// of a million fixes is one draw call at a constant pixel width.

import { mat4, v3 } from '../linalg.js';

const LINE_VS = `#version 300 es
layout(location=0) in vec2 aCorner;
layout(location=1) in vec3 aP0;
layout(location=2) in vec3 aP1;
layout(location=3) in float aV0;
layout(location=4) in float aV1;
uniform mat4 uMVP;
uniform vec2 uViewport;
uniform float uWidth;
out float vVal;
void main() {
  vec4 c0 = uMVP * vec4(aP0, 1.0);
  vec4 c1 = uMVP * vec4(aP1, 1.0);
  if (c0.w <= 0.0 && c1.w <= 0.0) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
  vec4 c = mix(c0, c1, aCorner.x);
  vec2 d = (c1.xy / c1.w - c0.xy / c0.w) * uViewport;
  vec2 n = dot(d, d) > 1e-12 ? normalize(vec2(-d.y, d.x)) : vec2(0.0, 1.0);
  c.xy += n * (uWidth * aCorner.y / uViewport) * c.w;
  vVal = mix(aV0, aV1, aCorner.x);
  gl_Position = c;
}`;

const POINT_VS = `#version 300 es
layout(location=1) in vec3 aP0;
layout(location=3) in float aV0;
uniform mat4 uMVP;
uniform float uWidth;
out float vVal;
void main() {
  gl_Position = uMVP * vec4(aP0, 1.0);
  gl_PointSize = uWidth;
  vVal = aV0;
}`;

const COLOR_FS = `#version 300 es
precision highp float;
in float vVal;
uniform vec3 uColor;
uniform int uMode;
uniform float uAlpha;
uniform int uRound;
out vec4 fragColor;
vec3 turbo(float x) {
  x = clamp(x, 0.0, 1.0);
  float r = 0.13572138 + x*(4.61539260 + x*(-42.66032258 + x*(132.13108234 + x*(-152.94239396 + x*59.28637943))));
  float g = 0.09140261 + x*(2.19418839 + x*(4.84296658 + x*(-14.18503333 + x*(4.27729857 + x*2.82956604))));
  float b = 0.10667330 + x*(12.64194608 + x*(-60.58204836 + x*(110.36276771 + x*(-89.90310912 + x*27.34824973))));
  return clamp(vec3(r, g, b), 0.0, 1.0);
}
void main() {
  if (uRound == 1) {
    vec2 d = gl_PointCoord - 0.5;
    if (dot(d, d) > 0.25) discard;
  }
  vec3 c = uMode == 1 ? turbo(vVal) : uColor;
  fragColor = vec4(c, uAlpha);
}`;

const TILE_VS = `#version 300 es
layout(location=0) in vec2 aCorner;
uniform mat4 uMVP;
uniform vec4 uRect;
uniform float uGround;
uniform vec3 uUV;
out vec2 vUv;
void main() {
  vec2 p = vec2(mix(uRect.x, uRect.z, aCorner.x), mix(uRect.y, uRect.w, aCorner.y));
  vUv = uUV.xy + aCorner * uUV.z;
  gl_Position = uMVP * vec4(p, uGround, 1.0);
}`;

const TILE_FS = `#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform float uAlpha;
uniform float uDesat;
out vec4 fragColor;
void main() {
  vec3 c = texture(uTex, vUv).rgb;
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  fragColor = vec4(mix(c, vec3(l), uDesat), uAlpha);
}`;

function compile(gl, vs, fs) {
  const mk = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
    return s;
  };
  const p = gl.createProgram();
  gl.attachShader(p, mk(gl.VERTEX_SHADER, vs));
  gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) {
    const name = gl.getActiveUniform(p, i).name.replace(/\[0\]$/, '');
    u[name] = gl.getUniformLocation(p, name);
  }
  return { p, u };
}

// Segment pairs for the instanced line program, as one buffer read at two
// offsets: instance i takes p0 from vertex i and p1 from vertex i+1.
class LineBuf {
  constructor(gl) { this.gl = gl; this.pos = gl.createBuffer(); this.val = gl.createBuffer(); this.n = 0; this.strip = true; }
  setStrip(pos, val) {
    const gl = this.gl;
    this.strip = true;
    this.n = pos.length / 3;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.pos);
    gl.bufferData(gl.ARRAY_BUFFER, pos, gl.STATIC_DRAW);
    this.setVal(val || new Float32Array(this.n));
  }
  // Independent segments: pos holds 2 vertices per segment.
  setSegments(pos, val) {
    this.setStrip(pos, val);
    this.strip = false;
  }
  setVal(val) {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.val);
    gl.bufferData(gl.ARRAY_BUFFER, val, gl.STATIC_DRAW);
  }
  instances() { return this.strip ? Math.max(0, this.n - 1) : Math.floor(this.n / 2); }
  stride() { return this.strip ? 12 : 24; }
  strideV() { return this.strip ? 4 : 8; }
  dispose() { this.gl.deleteBuffer(this.pos); this.gl.deleteBuffer(this.val); }
}

export class Scene {
  constructor(canvas) {
    const gl = canvas.getContext('webgl2', { antialias: true, alpha: false, preserveDrawingBuffer: true });
    if (!gl) throw new Error('WebGL2 is not available in this browser');
    this.gl = gl;
    this.canvas = canvas;
    this.line = compile(gl, LINE_VS, COLOR_FS);
    this.point = compile(gl, POINT_VS, COLOR_FS);
    this.tile = compile(gl, TILE_VS, TILE_FS);
    this.quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, -1, 0, 1, 1, -1, 1, 1]), gl.STATIC_DRAW);
    this.tileQuad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.tileQuad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 0, 1, 1, 0, 1, 1]), gl.STATIC_DRAW);
    this.vao = gl.createVertexArray();
    this.scratch = new LineBuf(gl);
    this.grid = new LineBuf(gl);
    this.gridKey = '';
  }

  bindLine(buf) {
    const gl = this.gl;
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(0, 0);
    const s = buf.stride(), sv = buf.strideV();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf.pos);
    for (const [loc, off] of [[1, 0], [2, 12]]) {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, s, off);
      gl.vertexAttribDivisor(loc, 1);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, buf.val);
    for (const [loc, off] of [[3, 0], [4, 4]]) {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, 1, gl.FLOAT, false, sv, off);
      gl.vertexAttribDivisor(loc, 1);
    }
  }

  drawLine(buf, mvp, color, width, mode, alpha) {
    if (!buf.instances()) return;
    const gl = this.gl, { p, u } = this.line;
    gl.useProgram(p);
    this.bindLine(buf);
    gl.uniformMatrix4fv(u.uMVP, false, mvp);
    gl.uniform2f(u.uViewport, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.uniform1f(u.uWidth, width);
    gl.uniform3fv(u.uColor, color);
    gl.uniform1i(u.uMode, mode | 0);
    gl.uniform1f(u.uAlpha, alpha === undefined ? 1 : alpha);
    gl.uniform1i(u.uRound, 0);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, buf.instances());
  }

  drawPoints(buf, mvp, color, size, mode, alpha, count) {
    const gl = this.gl, { p, u } = this.point;
    const n = count === undefined ? buf.n : count;
    if (!n) return;
    gl.useProgram(p);
    gl.bindVertexArray(this.vao);
    gl.disableVertexAttribArray(0);
    gl.bindBuffer(gl.ARRAY_BUFFER, buf.pos);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 12, 0);
    gl.vertexAttribDivisor(1, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, buf.val);
    gl.enableVertexAttribArray(3);
    gl.vertexAttribPointer(3, 1, gl.FLOAT, false, 4, 0);
    gl.vertexAttribDivisor(3, 0);
    gl.uniformMatrix4fv(u.uMVP, false, mvp);
    gl.uniform1f(u.uWidth, size);
    gl.uniform3fv(u.uColor, color);
    gl.uniform1i(u.uMode, mode | 0);
    gl.uniform1f(u.uAlpha, alpha === undefined ? 1 : alpha);
    gl.uniform1i(u.uRound, 1);
    gl.drawArrays(gl.POINTS, 0, n);
  }

  drawTiles(layer, mvp, ground, alpha, desat) {
    const gl = this.gl, { p, u } = this.tile;
    if (!layer.draws.length) return;
    gl.useProgram(p);
    gl.bindVertexArray(this.vao);
    for (let i = 1; i <= 4; i++) gl.disableVertexAttribArray(i);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.tileQuad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(0, 0);
    gl.uniformMatrix4fv(u.uMVP, false, mvp);
    gl.uniform1f(u.uGround, ground);
    gl.uniform1f(u.uAlpha, alpha);
    gl.uniform1f(u.uDesat, desat);
    gl.uniform1i(u.uTex, 0);
    gl.activeTexture(gl.TEXTURE0);
    for (const d of layer.draws) {
      gl.bindTexture(gl.TEXTURE_2D, d.tex);
      gl.uniform4f(u.uRect, d.rect[0], d.rect[1], d.rect[2], d.rect[3]);
      gl.uniform3f(u.uUV, d.uv[0], d.uv[1], d.uv[2]);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
  }

  // Power-of-10 cells that follow the zoom, on the global lattice so the
  // pattern does not swim while orbiting.
  updateGrid(center, dist, ground, axes) {
    const step = Math.pow(10, Math.round(Math.log10(dist / 6)));
    const key = `${step}|${Math.round(center[0] / step)}|${Math.round(center[1] / step)}|${ground}|${axes}`;
    if (key === this.gridKey) return step;
    this.gridKey = key;
    const half = 20;
    const cx = Math.round(center[0] / step) * step, cy = Math.round(center[1] / step) * step;
    const v = [];
    const push = (a, b) => { v.push(a[0], a[1], a[2], b[0], b[1], b[2]); };
    for (let i = -half; i <= half; i++) {
      const x = cx + i * step, y = cy + i * step;
      push([x, cy - half * step, ground], [x, cy + half * step, ground]);
      push([cx - half * step, y, ground], [cx + half * step, y, ground]);
    }
    this.grid.setSegments(new Float32Array(v), new Float32Array(v.length / 3));
    return step;
  }

  resize() {
    const c = this.canvas;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(c.clientWidth * dpr)), h = Math.max(1, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  }

  matrices(camera) {
    const gl = this.gl;
    const aspect = gl.drawingBufferWidth / Math.max(1, gl.drawingBufferHeight);
    const dist = Math.max(1e-3, camera.dist());
    const near = Math.max(0.05, dist * 0.004);
    const far = Math.max(2e5, dist * 200);
    const proj = mat4.perspective(camera.fov, aspect, near, far);
    const view = mat4.view(camera.rotMat3(), camera.pos);
    return { mvp: mat4.mul(proj, view), aspect, dist, near, far };
  }

  // Ground-plane footprint of the view: where the frustum corners land on the
  // plane, with rays that point at or above the horizon cut off at a far
  // distance so a horizon view asks for a bounded strip rather than a planet.
  groundBounds(camera, ground) {
    const gl = this.gl;
    const dist = Math.max(1e-3, camera.dist());
    const far = dist * 20;
    const ty = Math.tan(camera.fov / 2);
    const tx = ty * gl.drawingBufferWidth / Math.max(1, gl.drawingBufferHeight);
    const fwd = camera.forward(), right = camera.right(), up = camera.up(), pos = camera.pos;
    const lo = [Infinity, Infinity], hi = [-Infinity, -Infinity];
    const add = (p) => { for (let a = 0; a < 2; a++) { if (p[a] < lo[a]) lo[a] = p[a]; if (p[a] > hi[a]) hi[a] = p[a]; } };
    add(camera.target);
    for (const [sx, sy] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) {
      const d = v3.norm(v3.add(fwd, v3.add(v3.scale(right, sx * tx), v3.scale(up, sy * ty))));
      let t = d[2] < -1e-6 ? (ground - pos[2]) / d[2] : far;
      if (!(t > 0) || t > far) t = far;
      add(v3.add(pos, v3.scale(d, t)));
    }
    return [lo[0], lo[1], hi[0], hi[1]];
  }

  metresPerPixel(camera) {
    return 2 * Math.tan(camera.fov / 2) * Math.max(1e-3, camera.dist()) /
           Math.max(1, this.gl.drawingBufferHeight);
  }

  begin(bg) {
    const gl = this.gl;
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.clearColor(bg[0], bg[1], bg[2], 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  }
}

export { LineBuf };
