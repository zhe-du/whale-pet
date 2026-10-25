#!/usr/bin/env node
/**
 * dsh-pet 独立宿主（standalone host）
 * ============================================================================
 * 让「鲸鱼桌面宠物」脱离 DSH 网页进程独立运行：
 *
 *   1. 自带一个本地 HTTP 服务，提供桌宠渲染端需要的全部接口
 *      （/dsh-pet-7340/config、/thumb/<素材根>/<动画>.webm、/font、/pic、
 *        /balance、/work-status、/broadcast、/notify …）——接口契约与
 *      dsh-pet 宿主插件（lib/index.js 的 handlePetRoute）保持一致；
 *   2. 拉起 dsh-pet 自带的 Electron helper（runtime/electron-helper/main.js），
 *      注入 DSH_PET_CONFIG_URL / DSH_PET_PETS，不启用 bridge 模式（走裸 HTTP）；
 *   3. 常驻守护：Electron 退出自动重启、配置文件变化自动重启（与 DSH 的
 *      syncDesktop 行为对齐）；
 *   4. 可选：自动把 DSH 端桌宠配置的 display 改成 none，避免两套桌宠同时出现
 *      （DSH 的 helper 被杀会自动重启，所以必须从配置层面让 DSH 不再托管）。
 *
 * 启动：
 *   node server.mjs               前台启动（有控制台）
 *   node server.mjs --port 39471  指定端口
 *   node server.mjs --stop        停止已运行的独立实例
 *   node server.mjs --status      查看运行状态
 *
 * 环境变量：
 *   DSH_HOME                      ~/.dsh 根目录（默认 %USERPROFILE%\.dsh）
 *   DSH_PET_PACKAGE_ROOT          dsh-pet 包目录（默认自动探测 profiles/web/node_modules/dsh-pet）
 *   DSH_PET_ELECTRON_PATH         Electron 可执行文件（默认 ~/.dsh/electron/electron.exe）
 *   DSH_PET_STANDALONE_PORT       本地服务端口（默认 39471）
 *   DSH_WEB_PORT                  DSH Web 端口（默认 3080，用于自动纠正 display）
 *   DSH_PET_AUTOFIX_DSH=0         关闭「自动把 DSH 端 display 改成 none」
 *   DSH_PET_SMOKE=1               冒烟自检（把参数透传给 helper，9s 后截图并退出）
 *   DSH_PET_SMOKE_OUT             冒烟截图输出路径
 */

import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import {
  appendFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { homedir, platform } from 'node:os';
import { dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

// ---------------------------------------------------------------------------
// 基础常量
// ---------------------------------------------------------------------------

const SELF_DIR = dirname(fileURLToPath(import.meta.url));
const HOME = homedir();
const PLAT = platform();
const DSH_HOME = process.env.DSH_HOME ? resolve(process.env.DSH_HOME) : join(HOME, '.dsh');

/** dsh-pet 用户数据根（与宿主插件一致：$DSH_HOME/dsh-pet） */
const USER_ROOT = join(DSH_HOME, 'dsh-pet');
const USER_CONFIG = join(USER_ROOT, 'main-config.json');
const USER_PET_DIR = join(USER_ROOT, 'pet');
const USER_ANIM_ROOT = join(USER_ROOT, 'main-animation');
const STANDALONE_CONF = join(USER_ROOT, 'standalone.json');
/** pid 锁文件：优先用户数据目录；该目录不可写（受限沙箱/权限）时回落到程序目录 */
const PID_FILE = join(USER_ROOT, 'standalone.pid');
const PID_FILE_FALLBACK = join(SELF_DIR, 'logs', 'standalone.pid');

// 独立版专用 Chromium 用户目录（暂未启用）。
// 实测：与 DSH 自带 helper 共用 %APPDATA%\dsh-pet-electron-helper 时，后启动的那个会抢
// profile 的 LevelDB/SQLite 锁，30ms 内 CHECK 失败退出（0x80000003，stderr 一个字都没有）。
// 但 DSH 端桌宠已被改成 display=none，正常只有一套 helper 在跑，故默认沿用原路径；
// 若哪天要同时跑两套，把下面的 spawn 参数里补上 `--user-data-dir=${ELECTRON_PROFILE_DIR}` 即可。
const ELECTRON_PROFILE_DIR = join(process.env.LOCALAPPDATA || join(HOME, 'AppData', 'Local'), 'whale-pet-standalone', 'chromium');
const USE_DEDICATED_PROFILE = false;

/** 路由前缀（渲染端 constants.js 里写死 /dsh-pet-7340） */
const ROUTE_PREFIX = '/dsh-pet-7340';

const argv = process.argv.slice(2);
const flags = new Set(argv.filter((a) => a.startsWith('--')));
const argValue = (name) => {
  const hit = argv.find((a) => a === `--${name}` || a.startsWith(`--${name}=`));
  if (!hit) return '';
  return hit.includes('=') ? hit.slice(hit.indexOf('=') + 1) : '';
};

// 注意：不要用 process.env.PORT 兜底——DSH 子进程环境里带着 PORT=3080（DSH 自己的 Web 端口），
// 会与 DSH 抢端口。独立宿主只认自己的专用变量/参数。
const LOCAL_PORT = toPositiveInt(
  process.env.DSH_PET_STANDALONE_PORT || argValue('port'),
  39471,
);
const DSH_WEB_PORT = toPositiveInt(process.env.DSH_WEB_PORT, 3080);
const AUTO_FIX_DSH = process.env.DSH_PET_AUTOFIX_DSH !== '0';
const SMOKE = process.env.DSH_PET_SMOKE === '1';

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

const LOG_DIR = join(SELF_DIR, 'logs');
let LOG_FILE = '';

const pad2 = (n) => String(n).padStart(2, '0');
function stamp(d = new Date()) {
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
}
function clock(d = new Date()) {
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}.${String(d.getMilliseconds()).padStart(3, '0')}`;
}

function initLog() {
  try {
    mkdirSync(LOG_DIR, { recursive: true });
    LOG_FILE = join(LOG_DIR, `standalone-${stamp()}.log`);
    // 单日日志超过 8MB 就轮转一次（桌宠日志以 helper stderr 为主，长期运行会增长）
    if (existsSync(LOG_FILE) && statSync(LOG_FILE).size > 8 * 1024 * 1024) {
      renameSync(LOG_FILE, `${LOG_FILE}.1`);
    }
  } catch {
    LOG_FILE = '';
  }
}

function log(...parts) {
  const line = `[${clock()}] ${parts
    .map((p) => (typeof p === 'string' ? p : safeJson(p)))
    .join(' ')}`;
  if (LOG_FILE) {
    try {
      appendFileSync(LOG_FILE, line + '\n', 'utf8');
    } catch {
      /* 日志失败不能影响桌宠 */
    }
  }
  try {
    console.log(line);
  } catch {
    /* 隐藏窗口启动时 stdout 可能不可写 */
  }
}

function safeJson(value) {
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function toPositiveInt(raw, fallback) {
  const n = Number.parseInt(String(raw ?? ''), 10);
  return Number.isFinite(n) && n > 0 ? n : fallback;
}

// ---------------------------------------------------------------------------
// 包目录 / Electron 探测
// ---------------------------------------------------------------------------

/** dsh-pet 包根：优先 env，其次 profiles/web，再退化为扫描 profiles/* */
function findPackageRoot() {
  if (process.env.DSH_PET_PACKAGE_ROOT) return resolve(process.env.DSH_PET_PACKAGE_ROOT);
  const helperRel = join('runtime', 'electron-helper', 'main.js');
  const direct = join(DSH_HOME, 'profiles', 'web', 'node_modules', 'dsh-pet');
  if (existsSync(join(direct, helperRel))) return direct;
  const profiles = join(DSH_HOME, 'profiles');
  try {
    for (const name of readdirSync(profiles)) {
      const candidate = join(profiles, name, 'node_modules', 'dsh-pet');
      if (existsSync(join(candidate, helperRel))) return candidate;
    }
  } catch {
    /* 没有 profiles 目录就走 direct */
  }
  return direct;
}

const PACKAGE_ROOT = findPackageRoot();
const HELPER_MAIN = join(PACKAGE_ROOT, 'runtime', 'electron-helper', 'main.js');
const DEFAULT_CONFIG = join(PACKAGE_ROOT, 'assets', 'config.jsonc');

function electronRelPath() {
  if (PLAT === 'win32') return 'electron.exe';
  if (PLAT === 'darwin') return join('Electron.app', 'Contents', 'MacOS', 'Electron');
  return 'electron';
}

function findElectron() {
  const env = process.env.DSH_PET_ELECTRON_PATH;
  if (env && existsSync(env)) return env;
  const landing = join(DSH_HOME, 'electron', electronRelPath());
  return existsSync(landing) ? landing : '';
}

// ---------------------------------------------------------------------------
// JSONC / 配置合并（规则与 dsh-pet 宿主插件 lib/index.js 对齐）
// ---------------------------------------------------------------------------

/** 去掉 // 与 /* *\/ 注释（字符串内的注释符不动） */
function stripJsonComments(text) {
  let out = '';
  let i = 0;
  let inString = false;
  let inLine = false;
  let inBlock = false;
  while (i < text.length) {
    const c = text[i];
    const n = text[i + 1];
    if (inLine) {
      if (c === '\n') {
        inLine = false;
        out += c;
      }
      i += 1;
      continue;
    }
    if (inBlock) {
      if (c === '*' && n === '/') {
        inBlock = false;
        i += 2;
      } else i += 1;
      continue;
    }
    if (inString) {
      out += c;
      if (c === '\\') {
        out += n ?? '';
        i += 2;
        continue;
      }
      if (c === '"') inString = false;
      i += 1;
      continue;
    }
    if (c === '"') {
      inString = true;
      out += c;
      i += 1;
      continue;
    }
    if (c === '/' && n === '/') {
      inLine = true;
      i += 2;
      continue;
    }
    if (c === '/' && n === '*') {
      inBlock = true;
      i += 2;
      continue;
    }
    out += c;
    i += 1;
  }
  return out;
}

/** 宽松 JSONC 解析：去注释 + 去尾随逗号（配置里两种都出现过） */
function parseJsonc(text) {
  const cleaned = stripJsonComments(String(text)).replace(/,(\s*[}\]])/g, '$1');
  return JSON.parse(cleaned);
}

function readJsoncFile(file) {
  try {
    if (!existsSync(file)) return undefined;
    return parseJsonc(readFileSync(file, 'utf8'));
  } catch (e) {
    log(`配置解析失败：${file} → ${e instanceof Error ? e.message : String(e)}`);
    return undefined;
  }
}

const PET_DISPLAYS = new Set(['web', 'desktop', 'both', 'none']);
const CORNERS = new Set(['top-left', 'top-right', 'bottom-left', 'bottom-right', 'custom']);
const ID_FORBIDDEN = /[\\/:\x00-\x1f]/;

function petNumber(own, def, min) {
  const n = Number(own);
  if (own !== undefined && own !== null && Number.isFinite(n) && n >= min) return n;
  return Number(def);
}
function petBool(own, def) {
  return typeof own === 'boolean' ? own : Boolean(def);
}
function petEnum(own, set, def) {
  return typeof own === 'string' && set.has(own) ? own : typeof def === 'string' ? def : '';
}

/** 单只宠物规范化：id 全局唯一，其余字段缺失/非法 → 取内置默认（与宿主一致） */
function mergePet(base, raw, seenIds) {
  const p = raw && typeof raw === 'object' ? raw : {};
  const id = typeof p.id === 'string' ? p.id.trim() : '';
  if (!id || id.length > 64 || ID_FORBIDDEN.test(id) || seenIds.has(id)) return null;
  seenIds.add(id);
  const rawName = typeof p.name === 'string' ? p.name.trim() : '';
  const basePos = base.position && typeof base.position === 'object' ? base.position : {};
  const ownPos = p.position && typeof p.position === 'object' ? p.position : {};
  return {
    id,
    name: rawName || id,
    size: petNumber(p.size, base.size, 1),
    balanceEnabled: petBool(p.balanceEnabled, base.balanceEnabled),
    whisperEnabled: petBool(p.whisperEnabled, base.whisperEnabled),
    workStatusEnabled: petBool(p.workStatusEnabled, base.workStatusEnabled),
    display: petEnum(p.display, PET_DISPLAYS, base.display),
    position: {
      corner: petEnum(ownPos.corner, CORNERS, basePos.corner),
      marginX: petNumber(ownPos.marginX, basePos.marginX, -Infinity),
      marginY: petNumber(ownPos.marginY, basePos.marginY, -Infinity),
    },
  };
}

function mergePets(basePets, raw, seenIds) {
  const basePet = basePets[0] ?? {};
  if (!Array.isArray(raw) || raw.length === 0) return basePets;
  const out = [];
  for (const item of raw) {
    const pet = mergePet(basePet, item, seenIds);
    if (pet) out.push(pet);
  }
  return out.length > 0 ? out : basePets;
}

/** eventsRefreshSec 深度合并：缺键取默认，非法取默认 */
function mergeRefreshSec(base, overlay) {
  const baseSec = base && typeof base === 'object' ? base : {};
  const out = {};
  for (const [event, def] of Object.entries(baseSec)) {
    const own = overlay && typeof overlay === 'object' ? overlay[event] : undefined;
    const n = Number(own);
    out[event] = own !== undefined && Number.isFinite(n) && n > 0 ? n : Number(def);
  }
  return out;
}

/** 一个覆盖文件 → 完整条目（顶层逐字段覆盖，pets 逐只规范化） */
function mergeEntry(base, overlay, seenIds) {
  const out = {};
  for (const key of Object.keys(base)) {
    if (key === 'pets') {
      out.pets = mergePets(Array.isArray(base.pets) ? base.pets : [], overlay?.[key], seenIds);
      continue;
    }
    if (key === 'eventsRefreshSec') {
      out[key] = mergeRefreshSec(base[key], overlay?.[key]);
      continue;
    }
    const own = overlay ? overlay[key] : undefined;
    out[key] = own === undefined ? base[key] : own;
  }
  return out;
}

/** 用户自建宠物种类：$DSH_HOME/dsh-pet/pet/<前缀>-config.json */
function petConfigFiles() {
  try {
    if (!existsSync(USER_PET_DIR)) return [];
    return readdirSync(USER_PET_DIR)
      .filter((name) => name.endsWith('-config.json'))
      .sort()
      .map((name) => ({ prefix: name.slice(0, -'-config.json'.length), path: join(USER_PET_DIR, name) }));
  } catch {
    return [];
  }
}

/**
 * 合并出与宿主 GET /config 同构的成品配置。
 *
 * 与宿主的唯一差异：**所有宠物强制 display=desktop**。
 * 原因：独立化后 DSH 端配置会被改成 display=none（避免 DSH 再拉起一套桌宠），
 * 而渲染端会按 display 过滤桌面可见宠物；独立宿主自己就是"桌面端"，所以这里
 * 统一视为 desktop，配置里的 size/位置/余额开关仍照常跟随 main-config.json。
 */
function readAllConfig() {
  const base = readJsoncFile(DEFAULT_CONFIG);
  if (!base) throw new Error(`dsh-pet 内置默认配置缺失或解析失败：${DEFAULT_CONFIG}`);
  const seenIds = new Set();
  const out = {};
  out.main = mergeEntry(base, readJsoncFile(USER_CONFIG), seenIds);
  for (const file of petConfigFiles()) {
    const parsed = readJsoncFile(file.path);
    if (!parsed) continue;
    out[file.prefix] = mergeEntry(base, parsed, seenIds);
  }
  for (const conf of Object.values(out)) {
    if (Array.isArray(conf.pets)) {
      for (const pet of conf.pets) pet.display = 'desktop';
    }
  }
  return out;
}

/** 桌面宠物列表（顺序与渲染端 flattenConfigPets 一致：main 在前，pet 文件按名字排序） */
function desktopPetList() {
  const merged = readAllConfig();
  const out = [];
  for (const conf of Object.values(merged)) {
    if (!Array.isArray(conf.pets)) continue;
    for (const pet of conf.pets) out.push({ id: pet.id, size: pet.size });
  }
  return out;
}

// ---------------------------------------------------------------------------
// 素材路径（与宿主 /thumb、/font、/pic 一致）
// ---------------------------------------------------------------------------

function animSubdirFor(ext) {
  return ext === '.mov' ? 'mov' : 'webm';
}

/** 把相对路径安全地解析到 root 之下；不存在或越界返回 null */
function resolveUnder(root, rel) {
  const rootAbs = resolve(root);
  const target = resolve(rootAbs, rel);
  if (target !== rootAbs && !target.startsWith(rootAbs + sep)) return null;
  try {
    return existsSync(target) && statSync(target).isFile() ? target : null;
  } catch {
    return null;
  }
}

const MIME = {
  '.webm': 'video/webm',
  '.mov': 'video/quicktime',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.mp3': 'audio/mpeg',
  '.wav': 'audio/wav',
  '.json': 'application/json; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
};

// ---------------------------------------------------------------------------
// 余额（独立模式直接从 DSH 凭证文件取 key，直连 DeepSeek/OpenCode）
// ---------------------------------------------------------------------------

const FETCH_TIMEOUT_MS = 20_000;
const RETRIES = 3;

function readSettingsProvider() {
  const file = join(DSH_HOME, 'settings.yaml');
  const fallback = 'deepseek-official';
  if (!existsSync(file)) return fallback;
  try {
    const lines = readFileSync(file, 'utf8').split(/\r?\n/);
    let inBlock = false;
    for (const line of lines) {
      if (/^agent-default-model:\s*$/.test(line)) {
        inBlock = true;
        continue;
      }
      if (!inBlock) continue;
      if (/^\S/.test(line)) {
        inBlock = false;
        continue;
      }
      const m = /^\s+provider:\s*(\S+)\s*$/.exec(line);
      if (m) return unquoteYaml(m[1]);
    }
  } catch {
    /* 读失败回落默认 */
  }
  return fallback;
}

function unquoteYaml(value) {
  const text = String(value ?? '').trim();
  if (
    (text.startsWith('"') && text.endsWith('"')) ||
    (text.startsWith("'") && text.endsWith("'"))
  ) {
    return text.slice(1, -1);
  }
  return text;
}

/** 读 ~/.dsh/.credentials.yaml 的 refs.<name>（只解析这一种结构，够用且不引入 YAML 依赖） */
function readCredential(name) {
  const file = join(DSH_HOME, '.credentials.yaml');
  if (!existsSync(file)) return '';
  try {
    const lines = readFileSync(file, 'utf8').split(/\r?\n/);
    let inRefs = false;
    for (const line of lines) {
      if (/^refs:\s*$/.test(line)) {
        inRefs = true;
        continue;
      }
      if (!inRefs) continue;
      if (/^\S/.test(line)) {
        inRefs = false;
        continue;
      }
      const m = /^\s+([^:\s]+):\s*(.*)$/.exec(line);
      if (m && m[1] === name) return unquoteYaml(m[2]);
    }
  } catch {
    /* 读失败视为无凭证 */
  }
  return '';
}

async function fetchOnce(url, key) {
  return fetch(url, {
    headers: { Authorization: `Bearer ${key}`, 'User-Agent': 'dsh-pet-balance' },
    signal: AbortSignal.timeout(FETCH_TIMEOUT_MS),
  });
}

async function fetchWithRetry(url, key) {
  let last;
  for (let i = 0; i <= RETRIES; i += 1) {
    try {
      return await fetchOnce(url, key);
    } catch (e) {
      last = e;
      if (i < RETRIES) await sleep(800);
    }
  }
  throw last instanceof Error ? last : new Error(String(last));
}

function num(value, what) {
  const n = Number(value);
  if (!Number.isFinite(n)) throw new Error(`dsh-pet: 余额数据非法字段 ${what}`);
  return n;
}
function str(value, what) {
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`dsh-pet: 余额数据非法字段 ${what}`);
  }
  return value;
}

async function fetchOpencode(key, provider) {
  const res = await fetchWithRetry('https://opencode.ai/zen/go/v1/usage', key);
  if (!res.ok) throw new Error(`opencode usage HTTP ${res.status}`);
  const body = await res.json();
  const usage = body?.usage;
  if (!usage || typeof usage !== 'object') throw new Error('dsh-pet: opencode usage 响应缺少 usage');
  const { rolling, weekly, monthly } = usage;
  if (!rolling || !weekly || !monthly) throw new Error('dsh-pet: opencode usage 响应缺少窗口');
  return {
    ok: true,
    provider,
    kind: 'opencode',
    data: {
      rolling: num(rolling.percent, 'rolling.percent'),
      weekly: num(weekly.percent, 'weekly.percent'),
      monthly: num(monthly.percent, 'monthly.percent'),
      rollingResetsAt: str(rolling.resetsAt, 'rolling.resetsAt'),
      weeklyResetsAt: str(weekly.resetsAt, 'weekly.resetsAt'),
      monthlyResetsAt: str(monthly.resetsAt, 'monthly.resetsAt'),
    },
  };
}

async function fetchDeepseek(key, provider) {
  const res = await fetchWithRetry('https://api.deepseek.com/user/balance', key);
  if (!res.ok) throw new Error(`deepseek balance HTTP ${res.status}`);
  const body = await res.json();
  const infos = body?.balance_infos;
  if (!Array.isArray(infos) || infos.length === 0) {
    throw new Error('dsh-pet: deepseek balance 响应缺少 balance_infos');
  }
  const first = infos[0];
  return {
    ok: true,
    provider,
    kind: 'deepseek',
    data: {
      currency: str(first.currency, 'currency'),
      total: str(first.total_balance, 'total_balance'),
      granted: str(first.granted_balance, 'granted_balance'),
      toppedUp: str(first.topped_up_balance, 'topped_up_balance'),
    },
  };
}

const BALANCE_PROVIDERS = [
  { ids: ['opencode-go'], ref: 'OPENCODE_GO_API_KEY', kind: 'opencode' },
  { ids: ['deepseek-official'], ref: 'DEEPSEEK_API_KEY', kind: 'deepseek' },
];

async function queryBalance() {
  const provider = readSettingsProvider();
  const match = BALANCE_PROVIDERS.find((p) => p.ids.includes(provider));
  if (!match) return { ok: false, provider, reason: 'unsupported' };
  const key = readCredential(match.ref);
  if (!key) {
    return { ok: false, provider, reason: 'credential-missing', message: `缺少凭证 ${match.ref}` };
  }
  try {
    return match.kind === 'opencode'
      ? await fetchOpencode(key, provider)
      : await fetchDeepseek(key, provider);
  } catch (e) {
    return { ok: false, provider, reason: 'fetch-error', message: e instanceof Error ? e.message : String(e) };
  }
}

// ---------------------------------------------------------------------------
// HTTP 工具
// ---------------------------------------------------------------------------

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function corsHeaders(extra = {}) {
  return {
    'access-control-allow-origin': '*',
    'access-control-allow-methods': 'GET, POST, PUT, DELETE, OPTIONS',
    'access-control-allow-headers': 'content-type, range',
    'access-control-expose-headers': 'content-length, content-range, accept-ranges',
    'cache-control': 'no-store',
    ...extra,
  };
}

function sendJson(res, status, obj, extra = {}) {
  const body = JSON.stringify(obj);
  res.writeHead(status, corsHeaders({ 'content-type': 'application/json; charset=utf-8', ...extra }));
  res.end(body);
}

function sendText(res, status, text, extra = {}) {
  res.writeHead(status, corsHeaders({ 'content-type': 'text/plain; charset=utf-8', ...extra }));
  res.end(text);
}

/** 文件应答（支持 Range，视频/字体都能正常加载与拖动进度） */
function sendFile(req, res, file, contentType) {
  let size = 0;
  try {
    size = statSync(file).size;
  } catch {
    sendText(res, 404, 'dsh-pet: asset not found');
    return;
  }
  const headers = corsHeaders({
    'content-type': contentType,
    'accept-ranges': 'bytes',
    'cache-control': 'public, max-age=300',
  });
  const range = typeof req.headers.range === 'string' ? req.headers.range : '';
  const match = /^bytes=(\d*)-(\d*)$/.exec(range.trim());
  if (match) {
    let start = match[1] === '' ? NaN : Number(match[1]);
    let end = match[2] === '' ? NaN : Number(match[2]);
    if (!Number.isFinite(start)) {
      // 形如 bytes=-500：最后 500 字节
      const tail = Number.isFinite(end) ? end : 0;
      start = Math.max(0, size - tail);
      end = size - 1;
    } else {
      end = Number.isFinite(end) ? Math.min(end, size - 1) : size - 1;
    }
    if (start > end || start >= size) {
      res.writeHead(416, corsHeaders({ 'content-range': `bytes */${size}` }));
      res.end();
      return;
    }
    const chunk = readFileSync(file).subarray(start, end + 1);
    res.writeHead(206, {
      ...headers,
      'content-range': `bytes ${start}-${end}/${size}`,
      'content-length': String(chunk.length),
    });
    res.end(chunk);
    return;
  }
  const data = readFileSync(file);
  res.writeHead(200, { ...headers, 'content-length': String(data.length) });
  res.end(data);
}

// ---------------------------------------------------------------------------
// 路由（与宿主 handlePetRoute 的接口契约一致）
// ---------------------------------------------------------------------------

async function handlePetRoute(req, res, url) {
  const method = (req.method || 'GET').toUpperCase();
  const pathname = url.pathname;
  const rest = decodeURIComponent(pathname.slice(ROUTE_PREFIX.length).replace(/^\//, ''));

  if (rest === 'config') {
    if (method !== 'GET') return sendJson(res, 405, { error: 'method not allowed' });
    try {
      return sendJson(res, 200, readAllConfig());
    } catch (e) {
      return sendJson(res, 500, { error: e instanceof Error ? e.message : String(e) });
    }
  }

  // 渲染端 1s 轮询的统一状态源（官方 standalone 的同名接口）。
  // 缺了它会一直 400：渲染端拿不到状态就把窗口缩成错误条并挪到屏幕外
  // （-32000 / DPI 1.5 → -21333），表现为「桌宠凭空消失」。
  // 独立模式没有 DSH 的余额 / 工作状态能力，返回同构的空快照即可。
  if (rest === 'state') {
    if (method !== 'GET') return sendJson(res, 405, { error: 'method not allowed' });
    return sendJson(
      res,
      200,
      {
        sections: {
          balance: {
            counter: 0,
            data: { ok: false, provider: 'standalone', reason: 'unsupported' },
          },
          workStatus: { counter: 0, data: null },
          notify: { counter: 0, data: null },
        },
        pets: {},
      },
      { 'cache-control': 'no-cache, no-store' },
    );
  }

  if (rest === 'config/meta') {
    return sendJson(res, 200, {
      user: USER_CONFIG,
      default: DEFAULT_CONFIG,
      animations: USER_ANIM_ROOT,
      storage: [
        { key: 'userData', path: USER_ROOT, exists: existsSync(USER_ROOT) },
        { key: 'electron', path: join(DSH_HOME, 'electron'), exists: existsSync(join(DSH_HOME, 'electron')) },
        { key: 'package', path: PACKAGE_ROOT, exists: existsSync(PACKAGE_ROOT) },
        { key: 'standalone', path: SELF_DIR, exists: true },
      ],
      profile: 'standalone',
      standalone: true,
    });
  }

  if (rest === 'balance') {
    if (method !== 'GET') return sendJson(res, 405, { error: 'method not allowed' });
    const result = await queryBalance();
    return sendJson(res, 200, result);
  }

  if (rest === 'balance/trigger') {
    return sendJson(res, 200, { count: 0 }, { 'cache-control': 'no-cache, no-store' });
  }

  if (rest === 'whisper' || rest === 'whisper/trigger') {
    // 独立模式没有 DSH 的 LLM 能力：明确回不可用，渲染端会显示降级气泡而不是卡住
    return sendJson(res, 200, {
      ok: false,
      reason: 'standalone',
      message: '独立桌宠模式未接入碎碎念（需要 DSH 的模型能力）',
    });
  }

  if (rest === 'chat') {
    if (method === 'GET') return sendJson(res, 200, { ok: true, messages: [], rounds: 0 });
    return sendJson(res, 200, {
      ok: false,
      reason: 'standalone',
      message: '独立桌宠模式未接入对话（需要 DSH 的模型能力）',
    });
  }

  if (rest === 'broadcast') {
    return sendJson(res, 200, { ok: true, text: '', ts: 0 }, { 'cache-control': 'no-cache, no-store' });
  }

  if (rest === 'work-status') {
    return sendJson(res, 200, { state: null, task: null, ts: 0 }, { 'cache-control': 'no-cache, no-store' });
  }

  if (rest === 'notify') {
    return sendJson(res, 200, { ok: true, seq: 0, frames: [] }, { 'cache-control': 'no-cache, no-store' });
  }

  const [scope, ...restParts] = rest.split('/');

  if (scope === 'font') {
    const file = resolveUnder(join(PACKAGE_ROOT, 'assets', 'fonts'), restParts.join('/'));
    if (!file) return sendText(res, 404, 'dsh-pet: font not found');
    return sendFile(req, res, file, MIME[extname(file).toLowerCase()] ?? 'application/octet-stream');
  }

  if (scope === 'pic') {
    const isMeme = restParts[0] === 'memes';
    const root = join(PACKAGE_ROOT, 'assets', isMeme ? 'memes' : 'pic');
    const file = resolveUnder(root, (isMeme ? restParts.slice(1) : restParts).join('/'));
    if (!file) return sendText(res, 404, 'dsh-pet: pic not found');
    return sendFile(req, res, file, MIME[extname(file).toLowerCase()] ?? 'application/octet-stream');
  }

  if (scope !== 'thumb') return sendText(res, 400, 'dsh-pet: expected /dsh-pet-7340/thumb/<petId>/<file>');

  const [petId, ...nameParts] = restParts;
  if (!petId || nameParts.length === 0) {
    return sendText(res, 400, 'dsh-pet: expected /dsh-pet-7340/thumb/<petId>/<file>');
  }
  if (ID_FORBIDDEN.test(petId)) return sendText(res, 400, 'dsh-pet: invalid pet id');
  const fileName = nameParts.join('/');
  const ext = extname(fileName).toLowerCase();
  if (ext !== '.webm' && ext !== '.mov') {
    return sendText(res, 400, 'dsh-pet: unsupported animation format (expected .webm or .mov)');
  }
  // 优先级与宿主一致：pet 专属动画目录（存在即只认它）→ 用户覆盖目录 → 包内素材
  const extraAnimDir = join(USER_PET_DIR, `${petId}-animation`);
  let file = null;
  if (existsSync(extraAnimDir)) {
    file = resolveUnder(extraAnimDir, fileName);
  } else {
    file =
      resolveUnder(join(USER_ANIM_ROOT, animSubdirFor(ext)), fileName) ??
      resolveUnder(join(PACKAGE_ROOT, 'assets', animSubdirFor(ext)), fileName);
  }
  if (!file) return sendText(res, 404, 'dsh-pet: asset not found');
  return sendFile(req, res, file, MIME[ext] ?? 'application/octet-stream');
}

// ---------------------------------------------------------------------------
// 自动纠正 DSH 端配置（避免两套桌宠）
// ---------------------------------------------------------------------------

const DSH_ROUTE = `http://127.0.0.1:${DSH_WEB_PORT}${ROUTE_PREFIX}/config`;

/** 让 DSH 端不再显示桌面宠物：把 display=desktop|both 的实例改成 none（web/none 不动） */
async function fixDshDisplay(reason) {
  if (!AUTO_FIX_DSH) return false;
  let cfg;
  try {
    const res = await fetch(DSH_ROUTE, { signal: AbortSignal.timeout(2500) });
    if (!res.ok) return false;
    cfg = await res.json();
  } catch {
    return false; // DSH 没开（这正是独立模式的主场景）
  }
  const pets = cfg?.main?.pets;
  if (!Array.isArray(pets)) return false;
  const needs = pets.some((p) => p?.display === 'desktop' || p?.display === 'both');
  if (!needs) return false;
  const body = {
    pets: pets.map((p) => ({
      ...p,
      display: p?.display === 'desktop' || p?.display === 'both' ? 'none' : p?.display,
    })),
  };
  if (typeof cfg.main.notificationsEnabled === 'boolean') body.notificationsEnabled = cfg.main.notificationsEnabled;
  if (typeof cfg.main.whisperImageEnabled === 'boolean') body.whisperImageEnabled = cfg.main.whisperImageEnabled;
  if (typeof cfg.main.chatImageEnabled === 'boolean') body.chatImageEnabled = cfg.main.chatImageEnabled;
  try {
    const res = await fetch(DSH_ROUTE, {
      method: 'PUT',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body),
      signal: AbortSignal.timeout(6000),
    });
    if (!res.ok) {
      log(`DSH 配置纠正失败（HTTP ${res.status}）：${reason}`);
      return false;
    }
    log(`已把 DSH 端桌宠 display 改为 none（${reason}）——避免重复桌宠`);
    // DSH 收到 PUT 会 stopHelperAndWait 杀掉自己那套 helper；给它一点时间退干净
    await sleep(800);
    return true;
  } catch (e) {
    log(`DSH 配置纠正异常（${reason}）：${e instanceof Error ? e.message : String(e)}`);
    return false;
  }
}

// ---------------------------------------------------------------------------
// Electron 守护
// ---------------------------------------------------------------------------

class PetSupervisor {
  constructor({ electronPath }) {
    this.electronPath = electronPath;
    this.child = null;
    this.stopping = false;
    this.restartTimer = null;
    this.restartDelay = 3000;
    this.restarts = 0;
    this.fastCrashes = 0;
    this.lastStartAt = 0;
    this.stdoutBuf = '';
    this.stderrBuf = '';
  }

  start(reason = 'initial') {
    if (this.stopping || this.child) return;
    const env = {
      ...process.env,
      DSH_PET_CONFIG_URL: `http://127.0.0.1:${LOCAL_PORT}${ROUTE_PREFIX}/config`,
      DSH_PET_PETS: JSON.stringify(desktopPetList()),
      DSH_PET_SCALE: '1',
    };
    // 关键：不能带着 ELECTRON_RUN_AS_NODE（会让 Electron 退化成纯 Node、拿不到 electron 模块）
    delete env.ELECTRON_RUN_AS_NODE;
    // 不走 bridge：渲染端直接 HTTP 访问本服务
    delete env.DSH_PET_BRIDGE;
    // 注入宿主 PID：helper 每 2s 用 kill(pid,0) 探一次，只有确认 ESRCH 才退。
    // 独立模式下没开 bridge，stdout 几乎不写，EPIPE 守卫触发不到——这是宿主被强杀后
    // 桌宠能自己消失的唯一路径。
    env.DSH_PET_HOST_PID = String(process.pid);

    let pets = [];
    try {
      pets = JSON.parse(env.DSH_PET_PETS);
    } catch {
      /* 上面已是合法 JSON */
    }

    log(`启动 Electron helper（${reason}）：${this.electronPath}`);
    log(`  configUrl = ${env.DSH_PET_CONFIG_URL}`);
    if (USE_DEDICATED_PROFILE) log(`  profile   = ${ELECTRON_PROFILE_DIR}`);
    log(`  宠物 = ${safeJson(pets)}`);
    this.lastStartAt = Date.now();
    const args = [HELPER_MAIN];
    if (USE_DEDICATED_PROFILE) {
      try {
        mkdirSync(ELECTRON_PROFILE_DIR, { recursive: true });
      } catch {
        /* 建不出来就让 Electron 自己建 */
      }
      args.push(`--user-data-dir=${ELECTRON_PROFILE_DIR}`);
    }
    const child = spawn(this.electronPath, args, {
      cwd: PACKAGE_ROOT,
      env,
      stdio: ['ignore', 'pipe', 'pipe'],
      windowsHide: true,
    });
    this.child = child;

    child.once('error', (error) => {
      log(`Electron 启动失败：${error instanceof Error ? error.message : String(error)}`);
    });

    child.stdout?.on('data', (chunk) => this.consumeLines('stdout', chunk));
    child.stderr?.on('data', (chunk) => this.consumeLines('stderr', chunk));

    child.once('exit', (code, signal) => {
      if (this.child !== child) return;
      this.child = null;
      this.flushBuffers();
      if (this.stopping || SMOKE) {
        log(`Electron 已退出（code=${code} signal=${signal}）`);
        // 冒烟模式是一次性自检：helper 自检完退出后宿主也收工，方便命令行验证
        if (SMOKE) setTimeout(() => process.exit(0), 300);
        return;
      }
      // 秒退熔断：5s 内就挂不是抖动（profile 冲突 / 运行时缺失 / 显卡驱动），
      // 连挂 3 次就退出（码 3），交给启动器里的备用实现，别无限重启。
      const alive = Date.now() - this.lastStartAt;
      if (alive < 5000) {
        this.fastCrashes = (this.fastCrashes ?? 0) + 1;
        log(`Electron 秒退（存活 ${alive}ms，code=${code}），第 ${this.fastCrashes}/3 次`);
        if (this.fastCrashes >= 3) {
          log('Electron 连续 3 次秒退，本宿主退出（码 3）');
          process.exit(3);
        }
      } else {
        this.fastCrashes = 0;
      }
      log(`Electron 意外退出（code=${code} signal=${signal}），${Math.round(this.restartDelay / 1000)}s 后重启`);
      this.scheduleRestart();
    });
  }

  /** stdout/stderr 按行落日志（helper 的输出对排障很有用） */
  consumeLines(kind, chunk) {
    const text = String(chunk);
    const key = kind === 'stdout' ? 'stdoutBuf' : 'stderrBuf';
    const lines = (this[key] + text).split(/\r?\n/);
    this[key] = lines.pop() ?? '';
    for (const line of lines) {
      const trimmed = line.trim();
      if (trimmed) log(`[helper:${kind}] ${trimmed}`);
    }
  }

  flushBuffers() {
    for (const kind of ['stdout', 'stderr']) {
      const key = kind === 'stdout' ? 'stdoutBuf' : 'stderrBuf';
      const rest = this[key].trim();
      if (rest) log(`[helper:${kind}] ${rest}`);
      this[key] = '';
    }
  }

  scheduleRestart() {
    if (this.restartTimer) return;
    const delay = this.restartDelay;
    this.restartTimer = setTimeout(() => {
      this.restartTimer = null;
      if (this.stopping) return;
      this.restarts += 1;
      // 连续崩溃退避：3s → 6s → 12s → 24s → 30s 封顶
      this.restartDelay = Math.min(30000, this.restartDelay * (this.restarts > 3 ? 2 : 1));
      this.start(`自动重启#${this.restarts}`);
    }, delay);
  }

  restart(reason) {
    if (this.stopping) return;
    log(`重启 Electron：${reason}`);
    const old = this.child;
    this.child = null;
    if (old && !old.killed) {
      try {
        old.kill();
      } catch {
        /* 已退出 */
      }
    }
    if (this.restartTimer) {
      clearTimeout(this.restartTimer);
      this.restartTimer = null;
    }
    // 稍等旧进程收尾，避免新旧窗口短暂重叠
    setTimeout(() => {
      if (!this.stopping && !this.child) this.start(reason);
    }, 1200);
  }

  stop() {
    this.stopping = true;
    if (this.restartTimer) clearTimeout(this.restartTimer);
    this.restartTimer = null;
    const child = this.child;
    this.child = null;
    if (!child) return;
    try {
      child.kill();
    } catch {
      /* 忽略 */
    }
  }
}

// ---------------------------------------------------------------------------
// 配置变化监视
// ---------------------------------------------------------------------------

function configFingerprint() {
  const files = [DEFAULT_CONFIG, USER_CONFIG, ...petConfigFiles().map((f) => f.path)];
  const out = {};
  for (const file of files) {
    try {
      const st = statSync(file);
      out[file] = `${st.mtimeMs}:${st.size}`;
    } catch {
      out[file] = '0';
    }
  }
  return JSON.stringify(out);
}

// ---------------------------------------------------------------------------
// 单实例 / pid
// ---------------------------------------------------------------------------

function pidAlive(pid) {
  if (!(pid > 0)) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    // EPERM = 进程存在但无权操作（与 dsh-pet 的 host-liveness 同样的判定口径）
    return Boolean(e) && e.code === 'EPERM';
  }
}

function readPidFile() {
  for (const file of [PID_FILE, PID_FILE_FALLBACK]) {
    try {
      const raw = readFileSync(file, 'utf8').trim();
      const pid = Number.parseInt(raw, 10);
      if (Number.isFinite(pid) && pid > 0) return pid;
    } catch {
      /* 换下一个位置 */
    }
  }
  return 0;
}

function writePidFile() {
  for (const [dir, file] of [
    [USER_ROOT, PID_FILE],
    [LOG_DIR, PID_FILE_FALLBACK],
  ]) {
    try {
      mkdirSync(dir, { recursive: true });
      writeFileSync(file, String(process.pid), 'utf8');
      return;
    } catch {
      /* 换下一个位置 */
    }
  }
  log('写 pid 文件失败（不影响桌宠运行，只是单实例保护失效）');
}

function clearPidFile() {
  for (const file of [PID_FILE, PID_FILE_FALLBACK]) {
    try {
      if (Number.parseInt(readFileSync(file, 'utf8').trim(), 10) === process.pid) {
        rmSync(file, { force: true });
      }
    } catch {
      /* 不存在就算了 */
    }
  }
}

async function doStop() {
  const pid = readPidFile();
  if (!pid || !pidAlive(pid)) {
    console.log('独立桌宠没有在运行。');
    return 0;
  }
  try {
    process.kill(pid);
    console.log(`已请求停止独立桌宠（pid=${pid}）。`);
  } catch (e) {
    console.error(`停止失败：${e instanceof Error ? e.message : String(e)}`);
    return 1;
  }
  return 0;
}

function doStatus() {
  const pid = readPidFile();
  const running = pidAlive(pid);
  console.log(running ? `独立桌宠运行中（pid=${pid}，端口=${LOCAL_PORT}）` : '独立桌宠未运行。');
  return running ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

async function main() {
  initLog();

  if (flags.has('--stop')) return doStop();
  if (flags.has('--status')) return doStatus();

  // 单实例：已有实例在跑就直接退出（避免两只鲸鱼）
  const existing = readPidFile();
  if (existing && existing !== process.pid && pidAlive(existing)) {
    log(`已有独立桌宠在运行（pid=${existing}），本次启动退出。`);
    return 0;
  }

  log('============================================================');
  log('dsh-pet 独立宿主启动');
  log(`  DSH_HOME   = ${DSH_HOME}`);
  log(`  包目录     = ${PACKAGE_ROOT}`);
  log(`  Electron   = ${findElectron() || '(未找到)'}`);
  log(`  服务端口   = ${LOCAL_PORT}`);
  log(`  自动纠正DSH = ${AUTO_FIX_DSH ? '开' : '关'}`);
  log('============================================================');

  const electronPath = findElectron();
  if (!electronPath) {
    log('错误：找不到 Electron 运行时。请先运行一次 DSH 桌宠（会自动下载到 ~/.dsh/electron），');
    log('      或设置环境变量 DSH_PET_ELECTRON_PATH 指向 electron.exe。');
    return 1;
  }
  if (!existsSync(HELPER_MAIN)) {
    log(`错误：找不到 dsh-pet helper：${HELPER_MAIN}`);
    return 1;
  }

  writePidFile();

  // 启动前先让 DSH 端放弃桌宠（若 DSH 在线），等它把旧 helper 退干净再拉起自己的
  const fixed = await fixDshDisplay('启动前');
  if (fixed) await sleep(1200);

  // 本地资源服务
  const server = createServer((req, res) => {
    const started = Date.now();
    let url;
    try {
      url = new URL(req.url ?? '/', `http://127.0.0.1:${LOCAL_PORT}`);
    } catch {
      sendText(res, 400, 'bad request');
      return;
    }
    if ((req.method || 'GET').toUpperCase() === 'OPTIONS') {
      res.writeHead(204, corsHeaders());
      res.end();
      return;
    }
    if (url.pathname === '/') {
      // 右键菜单「打开网站」会打开服务 origin：转去 DSH，没有 DSH 时给个说明页
      res.writeHead(302, corsHeaders({ location: `http://127.0.0.1:${DSH_WEB_PORT}/` }));
      res.end();
      return;
    }
    if (!url.pathname.startsWith(`${ROUTE_PREFIX}/`) && url.pathname !== ROUTE_PREFIX) {
      sendText(res, 404, 'dsh-pet standalone: not found');
      return;
    }
    handlePetRoute(req, res, url)
      .catch((e) => {
        log(`路由异常 ${url.pathname}：${e instanceof Error ? e.message : String(e)}`);
        try {
          if (!res.headersSent) sendJson(res, 500, { error: e instanceof Error ? e.message : String(e) });
          else res.end();
        } catch {
          /* 连接已断 */
        }
      })
      .finally(() => {
        const ms = Date.now() - started;
        // 素材请求量大，只记录非 thumb 请求的耗时，避免日志刷屏
        if (!url.pathname.startsWith(`${ROUTE_PREFIX}/thumb/`)) {
          log(`HTTP ${req.method} ${url.pathname} → ${res.statusCode} (${ms}ms)`);
        }
      });
  });

  server.on('error', (error) => {
    log(`HTTP 服务错误：${error instanceof Error ? error.message : String(error)}`);
  });

  await new Promise((resolveListen) => {
    server.listen(LOCAL_PORT, '127.0.0.1', () => {
      log(`本地资源服务已监听 http://127.0.0.1:${LOCAL_PORT}${ROUTE_PREFIX}`);
      resolveListen();
    });
  });

  const supervisor = new PetSupervisor({ electronPath });
  supervisor.start(SMOKE ? 'smoke' : 'initial');
  if (SMOKE) log('冒烟模式：Electron 会在自检后自行退出（不自动重启）');

  // 配置文件变化 → 重启 Electron（对齐 DSH 的 syncDesktop：改大小/位置后即时生效）
  let lastFingerprint = configFingerprint();
  let watchTimer = null;
  const watch = setInterval(() => {
    const next = configFingerprint();
    if (next === lastFingerprint) return;
    lastFingerprint = next;
    if (SMOKE) return;
    if (watchTimer) clearTimeout(watchTimer);
    watchTimer = setTimeout(() => {
      watchTimer = null;
      supervisor.restart('配置文件变化');
    }, 1500);
  }, 2500);

  // 周期性检查 DSH 端是否又恢复了桌面宠物（例如用户在 DSH 设置页保存了配置）
  const guard = setInterval(() => {
    if (SMOKE) return;
    void fixDshDisplay('周期检查').then((didFix) => {
      if (didFix) {
        // 自己刚写过的配置不应触发「配置变化重启」
        lastFingerprint = configFingerprint();
      }
    });
  }, 20_000);

  const shutdown = (signal) => {
    log(`收到 ${signal}，正在退出…`);
    clearInterval(watch);
    clearInterval(guard);
    if (watchTimer) clearTimeout(watchTimer);
    supervisor.stop();
    clearPidFile();
    try {
      server.close();
    } catch {
      /* 忽略 */
    }
    setTimeout(() => process.exit(0), 600);
  };
  process.on('SIGINT', () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('uncaughtException', (error) => {
    log(`未捕获异常：${error instanceof Error ? error.stack ?? error.message : String(error)}`);
  });
  process.on('unhandledRejection', (reason) => {
    log(`未处理的 Promise 拒绝：${reason instanceof Error ? reason.stack ?? reason.message : String(reason)}`);
  });
  process.on('exit', () => clearPidFile());

  log('独立桌宠已就绪。按 Ctrl+C 或运行 `node server.mjs --stop` 退出。');
}

main()
  .then((code) => {
    if (typeof code === 'number' && code !== 0) process.exitCode = code;
  })
  .catch((error) => {
    log(`启动失败：${error instanceof Error ? error.stack ?? error.message : String(error)}`);
    process.exitCode = 1;
  });
