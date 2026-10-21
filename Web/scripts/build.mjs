import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const build = join(root, 'build');
const dist = join(root, 'dist');
rmSync(build, { recursive: true, force: true });
rmSync(dist, { recursive: true, force: true });
mkdirSync(build, { recursive: true });
mkdirSync(dist, { recursive: true });

const tsc = join(root, 'node_modules', 'typescript', 'bin', 'tsc');
if (!existsSync(tsc)) throw new Error('Dependencies are missing. Run pnpm install in Web/.');
execFileSync(process.execPath, [tsc, '-p', join(root, 'tsconfig.json')], { stdio: 'inherit' });

const js = readFileSync(join(build, 'app.js'));
const css = readFileSync(join(root, 'src', 'styles.css'));
const shortHash = data => createHash('sha256').update(data).digest('hex').slice(0, 12);
const jsName = `app.${shortHash(js)}.js`;
const cssName = `app.${shortHash(css)}.css`;
let html = readFileSync(join(root, 'src', 'index.html'), 'utf8').replace('__JS__', jsName).replace('__CSS__', cssName);
const htmlBytes = Buffer.from(html);
writeFileSync(join(dist, 'index.html'), htmlBytes);
writeFileSync(join(dist, jsName), js);
writeFileSync(join(dist, cssName), css);

const sourceAssets = [
  { path: '/index.html', data: htmlBytes, type: 1, cache: 0 },
  { path: `/${jsName}`, data: js, type: 2, cache: 1 },
  { path: `/${cssName}`, data: css, type: 3, cache: 1 }
];
const buildId = shortHash(Buffer.concat(sourceAssets.map(x => x.data)));
const entries = sourceAssets.map(asset => ({ ...asset, gzip: gzipSync(asset.data, { level: 9 }), etag: shortHash(asset.data) }));
const headerBytes = 32;
const entryBytes = 80;
const align = (value, boundary) => Math.ceil(value / boundary) * boundary;
let cursor = align(headerBytes + entryBytes * entries.length, 256);
for (const entry of entries) { entry.offset = cursor; cursor = align(cursor + entry.gzip.length, 256); }
if (cursor > 0x100000) throw new Error(`Web image is ${cursor} bytes; QSPI partition is 1 MiB`);
const image = Buffer.alloc(align(cursor, 4096), 0xff);
image.fill(0, 0, headerBytes + entryBytes * entries.length);
image.write('CSWB', 0, 'ascii');
image.writeUInt16LE(1, 4);
image.writeUInt16LE(entries.length, 6);
image.writeUInt32LE(image.length, 8);
image.writeUInt32LE(0, 12);
image.write(buildId.padEnd(16, '0'), 16, 16, 'ascii');
entries.forEach((entry, index) => {
  const base = headerBytes + index * entryBytes;
  image.write(entry.path, base, 47, 'ascii');
  image.writeUInt32LE(entry.offset, base + 48);
  image.writeUInt32LE(entry.gzip.length, base + 52);
  image.writeUInt32LE(entry.data.length, base + 56);
  image.write(entry.etag.padEnd(16, '0'), base + 60, 16, 'ascii');
  image[base + 76] = entry.type;
  image[base + 77] = entry.cache;
  entry.gzip.copy(image, entry.offset);
});
const crc32 = buffer => {
  let crc = 0xffffffff;
  for (const byte of buffer) { crc ^= byte; for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
  return (~crc) >>> 0;
};
const manifest = Buffer.from(image.subarray(0, headerBytes + entryBytes * entries.length));
manifest.writeUInt32LE(0, 12);
image.writeUInt32LE(crc32(manifest), 12);
writeFileSync(join(dist, 'qspi-web.bin'), image);
writeFileSync(join(dist, 'manifest.json'), JSON.stringify({ format: 1, build_id: buildId, qspi_offset: 0, image_bytes: image.length, assets: entries.map(({ path, offset, gzip, data, etag }) => ({ path, offset, compressed_bytes: gzip.length, original_bytes: data.length, etag })) }, null, 2));
console.log(`Built ${entries.length} gzip assets, QSPI image ${image.length} bytes, version ${buildId}`);
