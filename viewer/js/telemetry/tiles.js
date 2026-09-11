// Raster basemap for the 3D viewport: slippy-map tiles fetched straight from
// the provider and uploaded as textures, keyed by z/x/y with an LRU cache.
//
// A tile that has not arrived is drawn from its nearest loaded ancestor with
// a sub-rectangle of that texture, so panning and zooming never show holes.
// No provider key is needed for the built-ins; `custom` takes a {z}/{x}/{y}
// template so a keyed provider can be pasted in.

export const SOURCES = {
  osm:    { label: 'OpenStreetMap', url: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
            maxZoom: 19, credit: '© OpenStreetMap contributors' },
  esri:   { label: 'Satellite (Esri)', maxZoom: 19,
            url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
            credit: 'Imagery © Esri, Maxar, Earthstar Geographics' },
  dark:   { label: 'Dark (CARTO)', url: 'https://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png',
            maxZoom: 20, credit: '© OpenStreetMap contributors, © CARTO' },
  topo:   { label: 'Topographic', url: 'https://tile.opentopomap.org/{z}/{x}/{y}.png',
            maxZoom: 17, credit: '© OpenTopoMap (CC-BY-SA), © OpenStreetMap contributors' },
  custom: { label: 'Custom URL…', url: '', maxZoom: 22, credit: 'custom tile source' },
  none:   { label: 'None', url: '', maxZoom: 0, credit: '' },
};

const MAX_TEXTURES = 400;
const MAX_INFLIGHT = 8;

export class TileLayer {
  constructor(gl) {
    this.gl = gl;
    this.key = 'osm';
    this.customUrl = '';
    this.cache = new Map();      // "z/x/y" -> {tex, w, h} | 'failed'
    this.inflight = new Set();
    this.queue = [];
    this.draws = [];
    this.needRedraw = false;
  }

  source() { return SOURCES[this.key] || SOURCES.osm; }
  credit() { return this.key === 'none' ? '' : this.source().credit; }

  setSource(key, customUrl) {
    if (key === this.key && customUrl === this.customUrl) return;
    this.key = key;
    this.customUrl = customUrl || '';
    this.clear();
  }

  clear() {
    for (const v of this.cache.values()) if (v && v.tex) this.gl.deleteTexture(v.tex);
    this.cache.clear();
    this.queue.length = 0;
    this.draws.length = 0;
  }

  urlFor(z, x, y) {
    const s = this.source();
    const t = this.key === 'custom' ? this.customUrl : s.url;
    if (!t) return '';
    return t.replace('{z}', z).replace('{x}', x).replace('{y}', y)
            .replace('{s}', 'abc'[(x + y) % 3]);
  }

  request(z, x, y) {
    const key = `${z}/${x}/${y}`;
    if (this.cache.has(key) || this.inflight.has(key)) return;
    if (this.queue.includes(key)) return;
    this.queue.push(key);
    this.pump();
  }

  pump() {
    while (this.inflight.size < MAX_INFLIGHT && this.queue.length) {
      const key = this.queue.shift();
      const [z, x, y] = key.split('/').map(Number);
      const url = this.urlFor(z, x, y);
      if (!url) continue;
      this.inflight.add(key);
      fetch(url, { mode: 'cors', credentials: 'omit' })
        .then((r) => (r.ok ? r.blob() : Promise.reject(new Error(String(r.status)))))
        .then((b) => createImageBitmap(b))
        .then((img) => { this.upload(key, img); img.close(); })
        .catch(() => { this.cache.set(key, 'failed'); })
        .finally(() => { this.inflight.delete(key); this.needRedraw = true; this.pump(); });
    }
  }

  upload(key, img) {
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.cache.set(key, { tex });
    if (this.cache.size > MAX_TEXTURES) {
      for (const k of this.cache.keys()) {
        if (this.cache.size <= MAX_TEXTURES) break;
        const v = this.cache.get(k);
        if (v && v.tex) gl.deleteTexture(v.tex);
        this.cache.delete(k);
      }
    }
  }

  loaded(key) {
    const v = this.cache.get(key);
    return v && v !== 'failed' ? v : null;
  }

  // bounds: [west, south, east, north] in local metres; mpp: metres per screen
  // pixel at the ground plane. Builds this.draws for the frame.
  update(frame, bounds, mpp) {
    this.draws.length = 0;
    if (this.key === 'none' || (this.key === 'custom' && !this.customUrl)) return;
    const maxZ = this.source().maxZoom;
    let z = Math.round(Math.log2(frame.scale / (256 * Math.max(mpp, 1e-6))));
    z = Math.max(0, Math.min(maxZ, z));
    const n = 1 << z;

    const mx0 = frame.mx0 + bounds[0] / frame.scale, mx1 = frame.mx0 + bounds[2] / frame.scale;
    const my0 = frame.my0 - bounds[3] / frame.scale, my1 = frame.my0 - bounds[1] / frame.scale;
    const tx0 = Math.max(0, Math.floor(mx0 * n)), tx1 = Math.min(n - 1, Math.floor(mx1 * n));
    const ty0 = Math.max(0, Math.floor(my0 * n)), ty1 = Math.min(n - 1, Math.floor(my1 * n));
    if (tx1 < tx0 || ty1 < ty0) return;
    // A view that would need thousands of tiles is a view zoomed too far out
    // for this zoom level; step back until it is a few hundred.
    let zz = z, cx0 = tx0, cx1 = tx1, cy0 = ty0, cy1 = ty1;
    while (zz > 0 && (cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 256) {
      zz--; cx0 >>= 1; cx1 >>= 1; cy0 >>= 1; cy1 >>= 1;
    }
    const nz = 1 << zz;

    for (let ty = cy0; ty <= cy1; ty++) {
      for (let tx = cx0; tx <= cx1; tx++) {
        const key = `${zz}/${tx}/${ty}`;
        this.request(zz, tx, ty);
        let tile = this.loaded(key), ox = 0, oy = 0, sc = 1;
        for (let k = 1; !tile && zz - k >= 0 && k <= 8; k++) {
          const ax = tx >> k, ay = ty >> k;
          tile = this.loaded(`${zz - k}/${ax}/${ay}`);
          if (tile) { sc = 1 / (1 << k); ox = (tx - (ax << k)) * sc; oy = (ty - (ay << k)) * sc; }
          else if (k === 3) this.request(zz - k, ax, ay);
        }
        if (!tile) continue;
        const w = frame.mercToLocal(tx / nz, ty / nz);
        const e = frame.mercToLocal((tx + 1) / nz, (ty + 1) / nz);
        this.draws.push({ tex: tile.tex, rect: [w[0], w[1], e[0], e[1]], uv: [ox, oy, sc] });
      }
    }
  }
}
