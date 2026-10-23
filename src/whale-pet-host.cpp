// ============================================================================
// whale-pet-host.cpp —— 鲸鱼桌面宠物 独立宿主（C++ 版）
//
// 让 dsh-pet（DSH 的鲸鱼桌宠插件）脱离 DSH 进程独立运行：
//   1. 本地 HTTP 服务（127.0.0.1:39471）提供渲染端需要的全部接口；
//   2. 拉起 dsh-pet 自带的 Electron helper（runtime/electron-helper/main.js）；
//   3. 守护 helper：意外退出自动重启，配置变化自动重启，自身退出时带走 helper；
//   4. 自动把 DSH 端桌宠配置改成 display=none，避免两套桌宠同时出现；
//   5. --install 写开机自启（HKCU Run）+ 桌面快捷方式。
//
// 用法：
//   whale-pet-host.exe                启动桌宠（开机自启用这条）
//   whale-pet-host.exe --install      安装开机自启 + 桌面快捷方式
//   whale-pet-host.exe --uninstall    移除开机自启 + 桌面快捷方式
//   whale-pet-host.exe --stop         停止正在运行的桌宠
//   whale-pet-host.exe --status       查看状态（弹窗）
//   whale-pet-host.exe --smoke        冒烟自检（截图后退出）
//   whale-pet-host.exe --quiet        不弹窗（脚本/自动化用）
//   whale-pet-host.exe --port=39471   指定本地服务端口
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace {

// ---------------------------------------------------------------------------
// 常量 / 全局
// ---------------------------------------------------------------------------

constexpr const char* kRoutePrefix = "/dsh-pet-7340";
constexpr int kDefaultPort = 39471;
constexpr int kDefaultDshPort = 3080;
constexpr const wchar_t* kMutexName = L"Local\\dsh-pet-standalone-host";
constexpr const wchar_t* kStopEventName = L"Local\\dsh-pet-standalone-stop";
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"WhaleDesktopPet";
constexpr const wchar_t* kShortcutName = L"鲸鱼桌宠.lnk";

struct Paths {
  std::string exeDir;
  std::string dshHome;
  std::string userRoot;
  std::string userConfig;
  std::string userPetDir;
  std::string userAnimRoot;
  std::string packageRoot;
  std::string defaultConfig;
  std::string helperMain;
  std::string electron;
  std::string logDir;
  std::string profileDir;  // 独立版专用的 Chromium 用户目录（见 StartLocked 注释）
};

Paths g_paths;
std::mutex g_logMutex;
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_smoke{false};
std::atomic<int> g_exitCode{0};
bool g_autoFixDsh = true;
bool g_quiet = false;        // 只写日志，不弹任何窗
bool g_interactive = false;  // 前台交互命令（--install/--status/...）才允许弹窗
int g_port = kDefaultPort;
int g_dshPort = kDefaultDshPort;
HANDLE g_stopEvent = nullptr;
constexpr const char* kVersion = "1.1.0";

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

/** 日志超过 4MB 就滚存一份 .1（只保留一代），避免长期自启把日志写成几十 MB */
void RotateLogIfNeeded(const std::string& path) {
  static bool done = false;
  if (done) return;
  done = true;
  if (util::FileSize(path) < (4LL << 20)) return;
  const std::string old = path + ".1";
  DeleteFileW(util::Widen(old).c_str());
  MoveFileExW(util::Widen(path).c_str(), util::Widen(old).c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Log(const std::string& message) {
  const std::string line = "[" + util::NowClock() + "] " + message + "\n";
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_paths.logDir.empty()) {
      util::EnsureDir(g_paths.logDir);
      const std::string path = util::JoinPath(g_paths.logDir, "standalone-" + util::NowDate() + ".log");
      RotateLogIfNeeded(path);
      util::WriteFileUtf8(path, line, true);
    }
  }
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle && handle != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  }
}

/** 退出码转 8 位十六进制（0xC0000005 / 0x80000003 这种一眼能对上 Chromium 日志） */
std::string Hex32(DWORD value) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(value));
  return buf;
}

/**
 * 提示信息。
 *
 * ⚠️ 这里必须分两种语境：`--install/--status/--stop` 是用户亲手双击/敲命令调用的，弹个框告诉他
 * 结果很正常；但**宿主模式（开机自启、后台常驻）绝不能弹窗**——历史上就因为端口占用弹了一个
 * 没人看得见的模态框，进程一直挂在那里握着单实例互斥量，之后每次启动都只会得到
 * 「已有独立桌宠在运行」，而桌面上什么都没有。这类错误只写日志 + 退出码。
 */
void Notify(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
  Log(util::Narrow(text));
  if (g_quiet || !g_interactive) return;
  MessageBoxW(nullptr, text.c_str(), L"鲸鱼桌宠", MB_OK | icon);
}

/** 宿主模式的致命错误：写日志、不弹窗（除非显式要交互） */
int Fail(const std::string& message, const std::wstring& uiText = L"") {
  Log("错误：" + message);
  if (g_interactive && !g_quiet) {
    const std::wstring text = uiText.empty() ? util::Widen(message) : uiText;
    MessageBoxW(nullptr, text.c_str(), L"鲸鱼桌宠", MB_OK | MB_ICONERROR);
  }
  return 1;
}

std::string ExeEnvironment(const wchar_t* name) {
  std::wstring value(32768, L'\0');
  const DWORD len = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
  if (len == 0 || len >= value.size()) return std::string();
  value.resize(len);
  return util::Narrow(value);
}

// ---------------------------------------------------------------------------
// 路径初始化
// ---------------------------------------------------------------------------

std::string FindPackageRoot(const std::string& dshHome) {
  const std::string helperRel = "runtime\\electron-helper\\main.js";
  const std::string direct = util::JoinPath(dshHome, "profiles\\web\\node_modules\\dsh-pet");
  if (util::FileExists(util::JoinPath(direct, helperRel))) return direct;
  for (const std::string& name : util::ListDir(util::JoinPath(dshHome, "profiles"))) {
    const std::string candidate = util::JoinPath(util::JoinPath(dshHome, "profiles"), name + "\\node_modules\\dsh-pet");
    if (util::FileExists(util::JoinPath(candidate, helperRel))) return candidate;
  }
  return direct;
}

std::string FindElectron(const std::string& dshHome) {
  const std::string env = ExeEnvironment(L"DSH_PET_ELECTRON_PATH");
  if (!env.empty() && util::FileExists(env)) return env;
  const std::string landing = util::JoinPath(dshHome, "electron\\electron.exe");
  return util::FileExists(landing) ? landing : std::string();
}

void InitPaths() {
  g_paths.exeDir = util::ExeDir();
  std::string dshHome = ExeEnvironment(L"DSH_HOME");
  if (dshHome.empty()) dshHome = util::JoinPath(ExeEnvironment(L"USERPROFILE"), ".dsh");
  g_paths.dshHome = dshHome;
  g_paths.userRoot = util::JoinPath(dshHome, "dsh-pet");
  g_paths.userConfig = util::JoinPath(g_paths.userRoot, "main-config.json");
  g_paths.userPetDir = util::JoinPath(g_paths.userRoot, "pet");
  g_paths.userAnimRoot = util::JoinPath(g_paths.userRoot, "main-animation");
  g_paths.packageRoot = FindPackageRoot(dshHome);
  g_paths.defaultConfig = util::JoinPath(g_paths.packageRoot, "assets\\config.jsonc");
  g_paths.helperMain = util::JoinPath(g_paths.packageRoot, "runtime\\electron-helper\\main.js");
  g_paths.electron = FindElectron(dshHome);
  g_paths.logDir = util::JoinPath(g_paths.exeDir, "logs");
  // 独立版专用 Chromium 用户目录。
  // 不能和 DSH 自带的 helper 共用 %APPDATA%\dsh-pet-electron-helper：两个 Chromium 抢同一个
  // profile 的 LevelDB/SQLite 锁，后启动的那个会在 30ms 内 CHECK 失败退出（STATUS_BREAKPOINT，
  // 也就是日志里那个 2147483651，且一个字都不会写进 stderr）——这正是「C++ 版拉不起 Electron」的真因。
  const std::string localAppData = ExeEnvironment(L"LOCALAPPDATA");
  g_paths.profileDir = localAppData.empty() ? util::JoinPath(g_paths.userRoot, "standalone-profile")
                                            : util::JoinPath(localAppData, "whale-pet-standalone\\chromium");
}

// ---------------------------------------------------------------------------
// 配置读取 / 合并（规则对齐 dsh-pet 宿主插件 lib/index.js）
// ---------------------------------------------------------------------------

const std::vector<std::string> kDisplayValues = {"web", "desktop", "both", "none"};
const std::vector<std::string> kCornerValues = {"top-left", "top-right", "bottom-left", "bottom-right", "custom"};

bool InList(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

std::shared_ptr<js::Value> ReadJsonc(const std::string& path) {
  std::string text;
  if (!util::ReadFileUtf8(path, text)) return nullptr;
  std::string error;
  auto value = js::parse(text, &error);
  if (!value) {
    Log("配置解析失败：" + path + " → " + error);
    return nullptr;
  }
  return value;
}

double PetNumber(const js::Value* own, double def, double minValue) {
  if (own && own->isNum() && std::isfinite(own->num) && own->num >= minValue) return own->num;
  return def;
}

bool PetBool(const js::Value* own, bool def) { return own && own->isBool() ? own->b : def; }

std::string PetEnum(const js::Value* own, const std::vector<std::string>& allowed, const std::string& def) {
  if (own && own->isStr() && InList(allowed, own->str)) return own->str;
  return def;
}

std::shared_ptr<js::Value> MergePet(const js::Value& base, const js::Value& raw, std::vector<std::string>& seenIds) {
  std::string id = raw.isObj() && raw.get("id") && raw.get("id")->isStr() ? raw.get("id")->str : "";
  while (!id.empty() && (id.front() == ' ' || id.front() == '\t')) id.erase(id.begin());
  while (!id.empty() && (id.back() == ' ' || id.back() == '\t')) id.pop_back();
  if (id.empty() || id.size() > 64) return nullptr;
  if (id.find_first_of("\\/:") != std::string::npos) return nullptr;
  if (std::find(seenIds.begin(), seenIds.end(), id) != seenIds.end()) return nullptr;
  seenIds.push_back(id);

  const js::Value* rawName = raw.get("name");
  const std::string name = rawName && rawName->isStr() && !rawName->str.empty() ? rawName->str : id;
  const js::Value* basePos = base.get("position");
  const js::Value* ownPos = raw.get("position");

  auto pet = js::Value::object();
  pet->set("id", js::Value::string(id));
  pet->set("name", js::Value::string(name));
  pet->set("size", js::Value::number(PetNumber(raw.get("size"), base.get("size") ? base.get("size")->num : 440, 1)));
  pet->set("balanceEnabled",
           js::Value::boolean(PetBool(raw.get("balanceEnabled"), base.get("balanceEnabled") && base.get("balanceEnabled")->b)));
  pet->set("whisperEnabled",
           js::Value::boolean(PetBool(raw.get("whisperEnabled"), base.get("whisperEnabled") && base.get("whisperEnabled")->b)));
  pet->set("workStatusEnabled",
           js::Value::boolean(PetBool(raw.get("workStatusEnabled"), base.get("workStatusEnabled") && base.get("workStatusEnabled")->b)));
  pet->set("display",
           js::Value::string(PetEnum(raw.get("display"), kDisplayValues, base.get("display") ? base.get("display")->str : "desktop")));

  auto position = js::Value::object();
  const js::Value* ownCorner = ownPos ? ownPos->get("corner") : nullptr;
  const js::Value* baseCorner = basePos ? basePos->get("corner") : nullptr;
  position->set("corner", js::Value::string(PetEnum(ownCorner, kCornerValues, baseCorner && baseCorner->isStr() ? baseCorner->str : "top-right")));
  position->set("marginX", js::Value::number(PetNumber(ownPos ? ownPos->get("marginX") : nullptr,
                                                       basePos && basePos->get("marginX") ? basePos->get("marginX")->num : 24, -1e18)));
  position->set("marginY", js::Value::number(PetNumber(ownPos ? ownPos->get("marginY") : nullptr,
                                                       basePos && basePos->get("marginY") ? basePos->get("marginY")->num : 100, -1e18)));
  pet->set("position", position);
  return pet;
}

std::shared_ptr<js::Value> MergePets(const js::Value* basePets, const js::Value* raw, std::vector<std::string>& seenIds) {
  auto fallback = js::Value::array();
  if (basePets && basePets->isArr()) fallback = std::make_shared<js::Value>(*basePets);
  if (!raw || !raw->isArr() || raw->arr.empty()) return fallback;

  const js::Value* basePet = (basePets && basePets->isArr() && !basePets->arr.empty()) ? basePets->arr[0].get() : nullptr;
  auto out = js::Value::array();
  for (const auto& item : raw->arr) {
    auto pet = MergePet(basePet ? *basePet : js::Value(), *item, seenIds);
    if (pet) out->push(pet);
  }
  return out->arr.empty() ? fallback : out;
}

std::shared_ptr<js::Value> MergeRefreshSec(const js::Value* base, const js::Value* overlay) {
  auto out = js::Value::object();
  if (!base || !base->isObj()) return out;
  for (const auto& member : base->obj) {
    const js::Value* own = overlay && overlay->isObj() ? overlay->get(member.key) : nullptr;
    double value = member.value->isNum() ? member.value->num : 0;
    if (own && own->isNum() && std::isfinite(own->num) && own->num > 0) value = own->num;
    out->set(member.key, js::Value::number(value));
  }
  return out;
}

std::shared_ptr<js::Value> MergeEntry(const js::Value& base, const js::Value* overlay, std::vector<std::string>& seenIds) {
  auto out = js::Value::object();
  for (const auto& member : base.obj) {
    const std::string& key = member.key;
    if (key == "pets") {
      out->set(key, MergePets(member.value.get(), overlay && overlay->isObj() ? overlay->get(key) : nullptr, seenIds));
      continue;
    }
    if (key == "eventsRefreshSec") {
      out->set(key, MergeRefreshSec(member.value.get(), overlay && overlay->isObj() ? overlay->get(key) : nullptr));
      continue;
    }
    const js::Value* own = overlay && overlay->isObj() ? overlay->get(key) : nullptr;
    out->set(key, std::make_shared<js::Value>(own ? *own : *member.value));
  }
  return out;
}

struct PetConfigFile {
  std::string prefix;
  std::string path;
};

std::vector<PetConfigFile> PetConfigFiles() {
  std::vector<PetConfigFile> out;
  for (const std::string& name : util::ListDir(g_paths.userPetDir)) {
    if (name.size() > 12 && name.compare(name.size() - 12, 12, "-config.json") == 0) {
      out.push_back({name.substr(0, name.size() - 12), util::JoinPath(g_paths.userPetDir, name)});
    }
  }
  std::sort(out.begin(), out.end(), [](const PetConfigFile& a, const PetConfigFile& b) { return a.prefix < b.prefix; });
  return out;
}

/**
 * 与宿主 GET /config 同构的成品配置；唯一差异是所有宠物强制 display=desktop。
 * （独立化后 DSH 端配置是 none，而独立宿主自己就是桌面端。）
 */
js::Value ReadAllConfig() {
  auto base = ReadJsonc(g_paths.defaultConfig);
  if (!base || !base->isObj()) {
    Log("内置默认配置缺失或解析失败：" + g_paths.defaultConfig);
    return *js::Value::object();
  }
  auto overlay = ReadJsonc(g_paths.userConfig);

  std::vector<std::string> seenIds;
  auto merged = js::Value::object();
  merged->set("main", MergeEntry(*base, overlay.get(), seenIds));
  for (const auto& file : PetConfigFiles()) {
    auto parsed = ReadJsonc(file.path);
    if (!parsed || !parsed->isObj()) continue;
    merged->set(file.prefix, MergeEntry(*base, parsed.get(), seenIds));
  }
  for (auto& entry : merged->obj) {
    auto* pets = entry.value->get("pets");
    if (!pets || !pets->isArr()) continue;
    for (auto& pet : pets->arr) pet->set("display", js::Value::string("desktop"));
  }
  return *merged;
}

/** 桌面宠物列表（顺序与渲染端 flattenConfigPets 一致） */
std::string DesktopPetListJson() {
  const js::Value config = ReadAllConfig();
  auto list = js::Value::array();
  for (const auto& entry : config.obj) {
    const js::Value* pets = entry.value->get("pets");
    if (!pets || !pets->isArr()) continue;
    for (const auto& pet : pets->arr) {
      const js::Value* id = pet->get("id");
      const js::Value* size = pet->get("size");
      if (!id || !size) continue;
      auto item = js::Value::object();
      item->set("id", std::make_shared<js::Value>(*id));
      item->set("size", std::make_shared<js::Value>(*size));
      list->push(item);
    }
  }
  return js::stringify(*list);
}

// ---------------------------------------------------------------------------
// 素材路径
// ---------------------------------------------------------------------------

std::string ResolveUnder(const std::string& root, const std::string& rel) {
  if (rel.empty() || rel.size() > 1024) return std::string();
  if (rel[0] == '\\' || rel[0] == '/') return std::string();
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= rel.size()) {
    size_t pos = rel.find_first_of("\\/", start);
    if (pos == std::string::npos) pos = rel.size();
    const std::string part = rel.substr(start, pos - start);
    if (part == "..") return std::string();
    if (!part.empty() && part != ".") parts.push_back(part);
    start = pos + 1;
  }
  std::string joined;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) joined += '\\';
    joined += parts[i];
  }
  if (joined.empty()) return std::string();
  const std::string full = util::JoinPath(root, joined);
  return util::FileExists(full) ? full : std::string();
}

std::string AnimSubdirFor(const std::string& ext) { return ext == ".mov" ? "mov" : "webm"; }

const char* MimeFor(const std::string& ext) {
  static const std::map<std::string, const char*> table = {
      {".webm", "video/webm"}, {".mov", "video/quicktime"}, {".ttf", "font/ttf"},
      {".otf", "font/otf"},    {".woff", "font/woff"},      {".woff2", "font/woff2"},
      {".png", "image/png"},   {".jpg", "image/jpeg"},      {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},   {".webp", "image/webp"},     {".mp3", "audio/mpeg"},
      {".wav", "audio/wav"},   {".json", "application/json; charset=utf-8"},
      {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
  };
  auto it = table.find(ext);
  return it == table.end() ? "application/octet-stream" : it->second;
}

// ---------------------------------------------------------------------------
// WinHTTP（余额 / 纠正 DSH 配置）
// ---------------------------------------------------------------------------

struct HttpResponse {
  bool ok = false;
  int status = 0;
  std::string body;
  std::string error;
};

HttpResponse WinHttpRequest(const std::string& method, const std::string& url,
                            const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
                            int timeoutMs) {
  HttpResponse out;
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);

  std::wstring mutableUrl = util::Widen(url);
  if (!WinHttpCrackUrl(mutableUrl.c_str(), 0, 0, &components)) {
    out.error = "bad url";
    return out;
  }
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET session = WinHttpOpen(L"dsh-pet-standalone/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    out.error = "WinHttpOpen failed";
    return out;
  }
  WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

  HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (!connect) {
    out.error = "WinHttpConnect failed";
    WinHttpCloseHandle(session);
    return out;
  }
  const std::wstring wideMethod = util::Widen(method);
  HINTERNET request = WinHttpOpenRequest(connect, wideMethod.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    out.error = "WinHttpOpenRequest failed";
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
  }

  std::wstring headerBlock;
  for (const auto& [key, value] : headers) {
    headerBlock += util::Widen(key) + L": " + util::Widen(value) + L"\r\n";
  }
  const void* bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
  const DWORD bodyLen = static_cast<DWORD>(body.size());
  const BOOL sent = WinHttpSendRequest(request, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                       static_cast<DWORD>(headerBlock.size()), const_cast<void*>(bodyPtr), bodyLen, bodyLen, 0);
  if (!sent) {
    out.error = "WinHttpSendRequest failed";
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
  }
  if (!WinHttpReceiveResponse(request, nullptr)) {
    out.error = "WinHttpReceiveResponse failed";
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
  }
  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
  out.status = static_cast<int>(statusCode);

  std::string data;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
    chunk.resize(read);
    data += chunk;
    if (data.size() > 8 * 1024 * 1024) break;
  }
  out.body = std::move(data);
  out.ok = out.status >= 200 && out.status < 300;

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return out;
}

// ---------------------------------------------------------------------------
// 凭证 / 余额
// ---------------------------------------------------------------------------

std::string UnquoteYaml(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::string ReadSettingsProvider() {
  const std::string file = util::JoinPath(g_paths.dshHome, "settings.yaml");
  std::string text;
  if (!util::ReadFileUtf8(file, text)) return "deepseek-official";
  bool inBlock = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "agent-default-model:") {
      inBlock = true;
    } else if (inBlock) {
      if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
        inBlock = false;
      } else {
        const std::string prefix = "provider:";
        const size_t at = line.find(prefix);
        if (at != std::string::npos) return UnquoteYaml(line.substr(at + prefix.size()));
      }
    }
    if (end == text.size()) break;
    pos = end + 1;
  }
  return "deepseek-official";
}

std::string ReadCredential(const std::string& name) {
  const std::string file = util::JoinPath(g_paths.dshHome, ".credentials.yaml");
  std::string text;
  if (!util::ReadFileUtf8(file, text)) return std::string();
  bool inRefs = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "refs:") {
      inRefs = true;
    } else if (inRefs) {
      if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
        inRefs = false;
      } else {
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
          std::string key = line.substr(0, colon);
          while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
          if (key == name) return UnquoteYaml(line.substr(colon + 1));
        }
      }
    }
    if (end == text.size()) break;
    pos = end + 1;
  }
  return std::string();
}

std::string JsonError(const std::string& provider, const std::string& reason, const std::string& message) {
  auto obj = js::Value::object();
  obj->set("ok", js::Value::boolean(false));
  obj->set("provider", js::Value::string(provider));
  obj->set("reason", js::Value::string(reason));
  if (!message.empty()) obj->set("message", js::Value::string(message));
  return js::stringify(*obj);
}

std::string FetchDeepseekBalance(const std::string& key, const std::string& provider) {
  HttpResponse last;
  for (int attempt = 0; attempt <= 3; ++attempt) {
    last = WinHttpRequest("GET", "https://api.deepseek.com/user/balance",
                          {{"Authorization", "Bearer " + key},
                           {"User-Agent", "dsh-pet-balance"},
                           {"Accept", "application/json"}},
                          "", 20000);
    if (last.ok) break;
    util::SleepMs(800);
  }
  if (!last.ok) return JsonError(provider, "fetch-error", last.error + " (HTTP " + std::to_string(last.status) + ")");
  std::string error;
  auto body = js::parse(last.body, &error);
  const js::Value* infos = body ? body->get("balance_infos") : nullptr;
  if (!infos || !infos->isArr() || infos->arr.empty()) {
    return JsonError(provider, "fetch-error", "deepseek balance 响应缺少 balance_infos");
  }
  const js::Value& first = *infos->arr[0];
  auto readStr = [&first](const char* key) -> std::string {
    const js::Value* value = first.get(key);
    return value && value->isStr() ? value->str : std::string();
  };
  auto data = js::Value::object();
  data->set("currency", js::Value::string(readStr("currency")));
  data->set("total", js::Value::string(readStr("total_balance")));
  data->set("granted", js::Value::string(readStr("granted_balance")));
  data->set("toppedUp", js::Value::string(readStr("topped_up_balance")));
  auto obj = js::Value::object();
  obj->set("ok", js::Value::boolean(true));
  obj->set("provider", js::Value::string(provider));
  obj->set("kind", js::Value::string("deepseek"));
  obj->set("data", data);
  return js::stringify(*obj);
}

std::string QueryBalance() {
  const std::string provider = ReadSettingsProvider();
  if (provider == "opencode-go") return JsonError(provider, "unsupported", "独立版暂未接入 OpenCode 用量接口");
  if (provider != "deepseek-official") return JsonError(provider, "unsupported", "");
  const std::string key = ReadCredential("DEEPSEEK_API_KEY");
  if (key.empty()) return JsonError(provider, "credential-missing", "缺少凭证 DEEPSEEK_API_KEY");
  return FetchDeepseekBalance(key, provider);
}

// ---------------------------------------------------------------------------
// HTTP 服务
// ---------------------------------------------------------------------------

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

bool RecvMore(SOCKET sock, std::string& buffer) {
  char chunk[8192];
  const int got = recv(sock, chunk, sizeof(chunk), 0);
  if (got <= 0) return false;
  buffer.append(chunk, static_cast<size_t>(got));
  return buffer.size() <= (1u << 20);
}

bool ParseRequest(SOCKET sock, HttpRequest& out) {
  std::string buffer;
  size_t headerEnd = std::string::npos;
  for (;;) {
    headerEnd = buffer.find("\r\n\r\n");
    if (headerEnd != std::string::npos) break;
    if (buffer.size() > 65536) return false;
    if (!RecvMore(sock, buffer)) {
      if (buffer.empty()) return false;
      break;
    }
  }
  headerEnd = buffer.find("\r\n\r\n");
  if (headerEnd == std::string::npos) return false;

  const std::string head = buffer.substr(0, headerEnd);
  std::string rest = buffer.substr(headerEnd + 4);

  const size_t lineEnd = head.find("\r\n");
  const std::string requestLine = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
  const std::string headersText = lineEnd == std::string::npos ? std::string() : head.substr(lineEnd + 2);

  const size_t sp1 = requestLine.find(' ');
  if (sp1 == std::string::npos) return false;
  const size_t sp2 = requestLine.find(' ', sp1 + 1);
  out.method = requestLine.substr(0, sp1);
  const std::string target =
      sp2 == std::string::npos ? requestLine.substr(sp1 + 1) : requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
  const size_t q = target.find('?');
  out.path = q == std::string::npos ? target : target.substr(0, q);

  size_t pos = 0;
  while (pos < headersText.size()) {
    size_t end = headersText.find("\r\n", pos);
    if (end == std::string::npos) end = headersText.size();
    const std::string line = headersText.substr(pos, end - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      for (char& c : key) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
      out.headers[key] = value;
    }
    pos = end + 2;
  }

  size_t contentLength = 0;
  auto it = out.headers.find("content-length");
  if (it != out.headers.end()) contentLength = static_cast<size_t>(strtoull(it->second.c_str(), nullptr, 10));
  out.body = rest;
  while (out.body.size() < contentLength) {
    std::string more;
    if (!RecvMore(sock, more)) break;
    out.body += more;
  }
  if (out.body.size() > contentLength) out.body.resize(contentLength);
  return true;
}

void SendAll(SOCKET sock, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int rc = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (rc <= 0) return;
    sent += static_cast<size_t>(rc);
  }
}

std::string StatusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

const char* kCorsHeaders =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: content-type, range\r\n"
    "Access-Control-Expose-Headers: content-length, content-range, accept-ranges\r\n";

void SendJson(SOCKET sock, int status, const std::string& body, const std::string& extraHeaders = "") {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
  head += "Content-Type: application/json; charset=utf-8\r\n";
  head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  head += "Cache-Control: no-store\r\n";
  head += kCorsHeaders;
  head += extraHeaders;
  head += "Connection: close\r\n\r\n";
  SendAll(sock, head + body);
}

void SendText(SOCKET sock, int status, const std::string& text) {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
  head += "Content-Type: text/plain; charset=utf-8\r\n";
  head += "Content-Length: " + std::to_string(text.size()) + "\r\n";
  head += kCorsHeaders;
  head += "Connection: close\r\n\r\n";
  SendAll(sock, head + text);
}

void SendRedirect(SOCKET sock, const std::string& location) {
  std::string head = "HTTP/1.1 302 Found\r\n";
  head += "Location: " + location + "\r\n";
  head += "Content-Length: 0\r\n";
  head += kCorsHeaders;
  head += "Connection: close\r\n\r\n";
  SendAll(sock, head);
}

void SendFile(SOCKET sock, const std::string& path, const char* contentType, const std::string& rangeHeader) {
  const int64_t total = util::FileSize(path);
  if (total <= 0) {
    SendText(sock, 404, "dsh-pet: asset not found");
    return;
  }
  int64_t start = 0;
  int64_t end = total - 1;
  bool partial = false;
  if (!rangeHeader.empty() && rangeHeader.rfind("bytes=", 0) == 0) {
    const std::string spec = rangeHeader.substr(6);
    const size_t dash = spec.find('-');
    if (dash != std::string::npos) {
      const std::string a = spec.substr(0, dash);
      const std::string b = spec.substr(dash + 1);
      if (!a.empty()) {
        start = strtoll(a.c_str(), nullptr, 10);
        if (!b.empty()) end = strtoll(b.c_str(), nullptr, 10);
      } else if (!b.empty()) {
        start = total - strtoll(b.c_str(), nullptr, 10);
        if (start < 0) start = 0;
      }
      if (start > end || start >= total) {
        std::string head = "HTTP/1.1 416 Range Not Satisfiable\r\n";
        head += "Content-Range: bytes */" + std::to_string(total) + "\r\n";
        head += "Content-Length: 0\r\n";
        head += kCorsHeaders;
        head += "Connection: close\r\n\r\n";
        SendAll(sock, head);
        return;
      }
      if (end >= total) end = total - 1;
      partial = true;
    }
  }
  std::string data;
  if (!util::ReadFileBinary(path, data, static_cast<size_t>(start), static_cast<size_t>(end - start + 1))) {
    SendText(sock, 500, "dsh-pet: asset read failed");
    return;
  }
  std::string head = "HTTP/1.1 " + std::to_string(partial ? 206 : 200) + " " + StatusText(partial ? 206 : 200) + "\r\n";
  head += std::string("Content-Type: ") + contentType + "\r\n";
  head += "Content-Length: " + std::to_string(data.size()) + "\r\n";
  head += "Accept-Ranges: bytes\r\n";
  if (partial) {
    head += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total) + "\r\n";
  }
  head += "Cache-Control: public, max-age=300\r\n";
  head += kCorsHeaders;
  head += "Connection: close\r\n\r\n";
  SendAll(sock, head);
  SendAll(sock, data);
}

std::string UrlDecode(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(input[i + 1]);
      const int lo = hex(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += input[i];
  }
  return out;
}

std::vector<std::string> SplitPath(const std::string& text) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= text.size()) {
    size_t pos = text.find('/', start);
    if (pos == std::string::npos) pos = text.size();
    if (pos > start) parts.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

const std::string& RangeOf(const HttpRequest& req) {
  static const std::string empty;
  auto it = req.headers.find("range");
  return it == req.headers.end() ? empty : it->second;
}

void HandleRequest(SOCKET sock, const HttpRequest& req) {
  const std::string& method = req.method;
  const std::string& path = req.path;

  if (path == "/") {
    SendRedirect(sock, "http://127.0.0.1:" + std::to_string(g_dshPort) + "/");
    return;
  }
  if (path.compare(0, strlen(kRoutePrefix), kRoutePrefix) != 0) {
    SendText(sock, 404, "dsh-pet standalone: not found");
    return;
  }
  std::string rest = path.substr(strlen(kRoutePrefix));
  if (!rest.empty() && rest[0] == '/') rest.erase(rest.begin());

  if (rest == "config") {
    if (method != "GET") {
      SendJson(sock, 405, "{\"error\":\"method not allowed\"}");
      return;
    }
    SendJson(sock, 200, js::stringify(ReadAllConfig()));
    return;
  }
  if (rest == "config/meta") {
    auto meta = js::Value::object();
    meta->set("user", js::Value::string(g_paths.userConfig));
    meta->set("default", js::Value::string(g_paths.defaultConfig));
    meta->set("animations", js::Value::string(g_paths.userAnimRoot));
    meta->set("profile", js::Value::string("standalone"));
    meta->set("standalone", js::Value::boolean(true));
    SendJson(sock, 200, js::stringify(*meta));
    return;
  }
  if (rest == "balance") {
    if (method != "GET") {
      SendJson(sock, 405, "{\"error\":\"method not allowed\"}");
      return;
    }
    SendJson(sock, 200, QueryBalance());
    return;
  }
  if (rest == "balance/trigger") {
    SendJson(sock, 200, "{\"count\":0}", "Cache-Control: no-cache, no-store\r\n");
    return;
  }
  if (rest == "whisper" || rest == "whisper/trigger") {
    SendJson(sock, 200,
             "{\"ok\":false,\"reason\":\"standalone\",\"message\":\"独立桌宠模式未接入碎碎念（需要 DSH 的模型能力）\"}");
    return;
  }
  if (rest == "chat") {
    if (method == "GET") {
      SendJson(sock, 200, "{\"ok\":true,\"messages\":[],\"rounds\":0}");
    } else {
      SendJson(sock, 200,
               "{\"ok\":false,\"reason\":\"standalone\",\"message\":\"独立桌宠模式未接入对话（需要 DSH 的模型能力）\"}");
    }
    return;
  }
  if (rest == "broadcast") {
    SendJson(sock, 200, "{\"ok\":true,\"text\":\"\",\"ts\":0}", "Cache-Control: no-cache, no-store\r\n");
    return;
  }
  if (rest == "work-status") {
    SendJson(sock, 200, "{\"state\":null,\"task\":null,\"ts\":0}", "Cache-Control: no-cache, no-store\r\n");
    return;
  }
  if (rest == "notify") {
    SendJson(sock, 200, "{\"ok\":true,\"seq\":0,\"frames\":[]}", "Cache-Control: no-cache, no-store\r\n");
    return;
  }

  const std::vector<std::string> parts = SplitPath(rest);
  if (parts.empty()) {
    SendText(sock, 400, "dsh-pet: expected /dsh-pet-7340/thumb/<petId>/<file>");
    return;
  }
  const std::string& scope = parts[0];

  if (scope == "font") {
    std::string rel;
    for (size_t i = 1; i < parts.size(); ++i) {
      if (i > 1) rel += '/';
      rel += parts[i];
    }
    const std::string file = ResolveUnder(util::JoinPath(g_paths.packageRoot, "assets\\fonts"), UrlDecode(rel));
    if (file.empty()) {
      SendText(sock, 404, "dsh-pet: font not found");
      return;
    }
    SendFile(sock, file, MimeFor(util::Extension(file)), RangeOf(req));
    return;
  }
  if (scope == "pic") {
    const bool isMeme = parts.size() > 1 && parts[1] == "memes";
    std::string rel;
    for (size_t i = isMeme ? 2 : 1; i < parts.size(); ++i) {
      if (i > (isMeme ? 2u : 1u)) rel += '/';
      rel += parts[i];
    }
    const std::string root = util::JoinPath(g_paths.packageRoot, isMeme ? "assets\\memes" : "assets\\pic");
    const std::string file = ResolveUnder(root, UrlDecode(rel));
    if (file.empty()) {
      SendText(sock, 404, "dsh-pet: pic not found");
      return;
    }
    SendFile(sock, file, MimeFor(util::Extension(file)), RangeOf(req));
    return;
  }
  if (scope != "thumb" || parts.size() < 3) {
    SendText(sock, 400, "dsh-pet: expected /dsh-pet-7340/thumb/<petId>/<file>");
    return;
  }
  const std::string& petId = parts[1];
  if (petId.find_first_of("\\/:") != std::string::npos) {
    SendText(sock, 400, "dsh-pet: invalid pet id");
    return;
  }
  std::string rel;
  for (size_t i = 2; i < parts.size(); ++i) {
    if (i > 2) rel += '/';
    rel += parts[i];
  }
  const std::string fileName = UrlDecode(rel);
  const std::string ext = util::Extension(fileName);
  if (ext != ".webm" && ext != ".mov") {
    SendText(sock, 400, "dsh-pet: unsupported animation format (expected .webm or .mov)");
    return;
  }
  const std::string extraAnimDir = util::JoinPath(g_paths.userPetDir, petId + "-animation");
  std::string file;
  if (util::DirExists(extraAnimDir)) {
    file = ResolveUnder(extraAnimDir, fileName);
  } else {
    file = ResolveUnder(util::JoinPath(g_paths.userAnimRoot, AnimSubdirFor(ext)), fileName);
    if (file.empty()) {
      file = ResolveUnder(util::JoinPath(g_paths.packageRoot, std::string("assets\\") + AnimSubdirFor(ext)), fileName);
    }
  }
  if (file.empty()) {
    SendText(sock, 404, "dsh-pet: asset not found");
    return;
  }
  SendFile(sock, file, MimeFor(ext), RangeOf(req));
}

class HttpServer {
 public:
  const std::string& LastError() const { return lastError_; }

  bool Start(int port) {
    lastError_.clear();
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      lastError_ = "WSAStartup 失败";
      return false;
    }
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) {
      lastError_ = "socket 创建失败（WinSock 提供者不可用，错误码 " + std::to_string(WSAGetLastError()) +
                   "；可尝试管理员执行 netsh winsock reset 后重启）";
      return false;
    }
    const BOOL exclusive = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      lastError_ = "端口 " + std::to_string(port) + " 已被占用";
      closesocket(listen_);
      listen_ = INVALID_SOCKET;
      return false;
    }
    if (listen(listen_, 32) != 0) {
      lastError_ = "listen 失败（错误码 " + std::to_string(WSAGetLastError()) + "）";
      closesocket(listen_);
      listen_ = INVALID_SOCKET;
      return false;
    }
    running_ = true;
    acceptThread_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  void Stop() {
    running_ = false;
    if (listen_ != INVALID_SOCKET) {
      closesocket(listen_);
      listen_ = INVALID_SOCKET;
    }
    if (acceptThread_.joinable()) acceptThread_.join();
  }

 private:
  void AcceptLoop() {
    while (running_) {
      SOCKET client = accept(listen_, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        if (!running_) break;
        continue;
      }
      std::thread([client] {
        HttpRequest req;
        if (ParseRequest(client, req)) HandleRequest(client, req);
        shutdown(client, SD_SEND);
        closesocket(client);
      }).detach();
    }
  }

  SOCKET listen_ = INVALID_SOCKET;
  std::atomic<bool> running_{false};
  std::thread acceptThread_;
  std::string lastError_;
};

HttpServer g_server;

// ---------------------------------------------------------------------------
// Electron 守护（stdin/stdout 走管道，与 Node 的 spawn 行为一致）
// ---------------------------------------------------------------------------

/** 管道读线程：按行写入宿主日志 */
void PumpPipe(HANDLE readHandle, const std::string& tag) {
  std::string buffer;
  char chunk[4096];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(readHandle, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
    buffer.append(chunk, read);
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) Log(tag + line);
    }
  }
  if (!buffer.empty()) Log(tag + buffer);
  CloseHandle(readHandle);
}

void ApplyChildEnvironment() {
  SetEnvironmentVariableW(L"ELECTRON_RUN_AS_NODE", nullptr);
  SetEnvironmentVariableW(L"DSH_PET_BRIDGE", nullptr);
  SetEnvironmentVariableW(L"DSH_PET_SMOKE", nullptr);
  SetEnvironmentVariableW(L"DSH_PET_SMOKE_OUT", nullptr);
  SetEnvironmentVariableW(L"DSH_PET_SMOKE_AFTER_MS", nullptr);
  // 注入宿主 PID：helper 每 2s 用 kill(pid,0) 探一次，**只有确认 ESRCH 才退**。
  // 这是「宿主被强杀 → 桌宠留在屏幕上关不掉」的唯一收敛路径（独立模式下没开 bridge，
  // stdout 几乎不写，EPIPE 守卫基本触发不到）。
  SetEnvironmentVariableW(L"DSH_PET_HOST_PID", std::to_wstring(GetCurrentProcessId()).c_str());
  SetEnvironmentVariableW(L"DSH_PET_CONFIG_URL",
                          util::Widen("http://127.0.0.1:" + std::to_string(g_port) + kRoutePrefix + "/config").c_str());
  SetEnvironmentVariableW(L"DSH_PET_PETS", util::Widen(DesktopPetListJson()).c_str());
  SetEnvironmentVariableW(L"DSH_PET_SCALE", L"1");
  if (g_smoke) {
    SetEnvironmentVariableW(L"DSH_PET_SMOKE", L"1");
    SetEnvironmentVariableW(L"DSH_PET_SMOKE_OUT", util::Widen(util::JoinPath(g_paths.exeDir, "smoke.png")).c_str());
    SetEnvironmentVariableW(L"DSH_PET_SMOKE_AFTER_MS", L"9000");
  }
}

class Supervisor {
 public:
  void Start(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load() || process_) return;
    StartLocked(reason);
  }

  void Restart(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load()) return;
    Log("重启 Electron：" + reason);
    KillLocked();
    StartLocked(reason);
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    KillLocked();
  }

 private:
  void StartLocked(const std::string& reason) {
    if (g_paths.electron.empty() || !util::FileExists(g_paths.helperMain)) {
      Log("无法启动 Electron：路径缺失（electron=" + g_paths.electron + "）");
      return;
    }
    util::EnsureDir(g_paths.logDir);
    util::EnsureDir(g_paths.profileDir);
    ApplyChildEnvironment();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr, nul = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
      Log("创建管道失败：" + std::to_string(GetLastError()));
      return;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);

    // 独立 profile：与 DSH 自带的 helper 彻底隔开（否则抢 profile 锁 → 30ms 内 0x80000003 退出）
    std::wstring cmd = L"\"" + util::Widen(g_paths.electron) + L"\" \"" + util::Widen(g_paths.helperMain) + L"\"" +
                       L" --user-data-dir=\"" + util::Widen(g_paths.profileDir) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};

    Log("启动 Electron helper（" + reason + "）：" + g_paths.electron);
    Log("  configUrl = http://127.0.0.1:" + std::to_string(g_port) + kRoutePrefix + "/config");
    Log("  profile   = " + g_paths.profileDir);
    Log("  宠物 = " + DesktopPetListJson());

    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                                        nullptr, util::Widen(g_paths.packageRoot).c_str(), &si, &pi);
    // 父进程必须关掉写端，否则读端永远等不到 EOF
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) {
      Log("CreateProcess 失败：错误码 " + std::to_string(GetLastError()));
      CloseHandle(outRead);
      CloseHandle(errRead);
      return;
    }

    // 注意：这里刻意**不**把 Electron 放进 Job Object。
    // DSH 启动本程序时通常已把本进程放进它自己的 job；再给 Electron 套一层嵌套 job，
    // Chromium 的自沙箱初始化会 CHECK 失败（STATUS_BREAKPOINT=0x80000003）直接退出。
    // 子进程的生命周期改由「管道断开 → helper 自退」与 Stop() 的 TerminateProcess 兜底。

    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    childPid_ = pi.dwProcessId;
    const ULONGLONG launchedAt = GetTickCount64();

    std::thread(PumpPipe, outRead, std::string("[helper:out] ")).detach();
    std::thread(PumpPipe, errRead, std::string("[helper:err] ")).detach();

    // 退出监控：区分「主动停止」与「意外退出」
    std::thread([this, process = process_, launchedAt] {
      WaitForSingleObject(process, INFINITE);
      DWORD code = 0;
      GetExitCodeProcess(process, &code);
      const bool intentional = intentionalKill_.exchange(false);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (process_ == process) {
          process_ = nullptr;
          CloseHandle(process);
        }
      }
      if (g_smoke && !intentional) {
        Log("冒烟模式：Electron 已退出（code=" + std::to_string(code) + "），宿主收工");
        g_exitCode = code == 0 ? 0 : 2;
        if (g_stopEvent) SetEvent(g_stopEvent);
        return;
      }
      if (intentional || stopping_.load() || g_stopping.load()) {
        Log("Electron 已退出（code=" + std::to_string(code) + "）");
        return;
      }
      // 秒退熔断：5 秒内就挂说明不是偶发抖动（profile 冲突 / 运行时缺失 / 显卡驱动），
      // 继续 3s→6s→12s 地重试只会白烧 CPU 并让用户看不到任何东西。
      // 连挂 3 次就直接退出（退出码 3），把舞台交给 start.vbs 里的 Node 备用宿主。
      const ULONGLONG alive = GetTickCount64() - launchedAt;
      if (alive < 5000) {
        const int crashes = ++rapidCrashes_;
        Log("Electron 秒退（存活 " + std::to_string(alive) + "ms，code=0x" + Hex32(code) + "），第 " +
            std::to_string(crashes) + "/3 次");
        if (crashes >= 3) {
          Log("Electron 连续 3 次秒退，放弃本宿主（退出码 3）；若装有 Node 备用宿主，启动器会自动接管");
          g_exitCode = 3;
          if (g_stopEvent) SetEvent(g_stopEvent);
          return;
        }
      } else {
        rapidCrashes_ = 0;
      }
      Log("Electron 意外退出（code=" + std::to_string(code) + "），" + std::to_string(restartDelayMs_ / 1000) + "s 后重启");
      const int delay = restartDelayMs_;
      restartDelayMs_ = std::min(30000, restartDelayMs_ * 2);
      std::thread([this, delay] {
        util::SleepMs(delay);
        bool stillCurrent = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          stillCurrent = (process_ == nullptr) && !stopping_.load();
        }
        if (!stillCurrent) return;
        Start("自动重启");
      }).detach();
    }).detach();
  }

  void KillLocked() {
    if (!process_) return;
    Log("停止 Electron（pid=" + std::to_string(childPid_) + "）");
    intentionalKill_ = true;
    HANDLE p = process_;
    process_ = nullptr;
    TerminateProcess(p, 0);
    WaitForSingleObject(p, 3000);
    CloseHandle(p);
  }

  std::mutex mutex_;
  HANDLE process_ = nullptr;
  DWORD childPid_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> intentionalKill_{false};
  std::atomic<int> rapidCrashes_{0};
  int restartDelayMs_ = 3000;
};

Supervisor g_supervisor;

// ---------------------------------------------------------------------------
// 自动纠正 DSH 端配置
// ---------------------------------------------------------------------------

bool FixDshDisplay(const std::string& reason) {
  if (!g_autoFixDsh) return false;
  const std::string url = "http://127.0.0.1:" + std::to_string(g_dshPort) + kRoutePrefix + "/config";
  HttpResponse get = WinHttpRequest("GET", url, {}, "", 2500);
  if (!get.ok) return false;

  std::string error;
  auto config = js::parse(get.body, &error);
  const js::Value* main = config ? config->get("main") : nullptr;
  const js::Value* pets = main ? main->get("pets") : nullptr;
  if (!pets || !pets->isArr()) return false;

  bool needs = false;
  for (const auto& pet : pets->arr) {
    const js::Value* display = pet->get("display");
    if (display && display->isStr() && (display->str == "desktop" || display->str == "both")) {
      needs = true;
      break;
    }
  }
  if (!needs) return false;

  auto body = js::Value::object();
  auto newPets = js::Value::array();
  for (const auto& pet : pets->arr) {
    auto copy = std::make_shared<js::Value>(*pet);
    const js::Value* display = pet->get("display");
    if (display && display->isStr() && (display->str == "desktop" || display->str == "both")) {
      copy->set("display", js::Value::string("none"));
    }
    newPets->push(copy);
  }
  body->set("pets", newPets);
  for (const char* key : {"notificationsEnabled", "whisperImageEnabled", "chatImageEnabled"}) {
    const js::Value* value = main->get(key);
    if (value && value->isBool()) body->set(key, std::make_shared<js::Value>(*value));
  }

  HttpResponse put = WinHttpRequest("PUT", url, {{"Content-Type", "application/json"}}, js::stringify(*body), 6000);
  if (!put.ok) {
    Log("DSH 配置纠正失败（HTTP " + std::to_string(put.status) + "）：" + reason);
    return false;
  }
  Log("已把 DSH 端桌宠 display 改为 none（" + reason + "）——避免重复桌宠");
  util::SleepMs(800);
  return true;
}

// ---------------------------------------------------------------------------
// 单实例 / 停止 / 状态
// ---------------------------------------------------------------------------

bool AnotherInstanceRunning() {
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
  if (!mutex) return false;
  CloseHandle(mutex);
  return true;
}

int CmdStop() {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
  if (!event) {
    Notify(L"独立桌宠没有在运行。");
    return 0;
  }
  SetEvent(event);
  CloseHandle(event);
  Notify(L"已请求停止独立桌宠。");
  return 0;
}

/** 统计某个镜像名的进程数（用来看 Electron 起了几个：主进程 + gpu + network + renderer） */
int CountProcesses(const std::wstring& imageName) {
  int count = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, imageName.c_str()) == 0) ++count;
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return count;
}

std::wstring AutostartState() {
  // 机制一：HKCU\...\Run
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    wchar_t value[1024] = {0};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) return std::wstring(L"已安装（注册表 Run：") + value + L"）";
  }
  // 机制二：启动文件夹里的快捷方式（install-autostart.ps1 装的就是这个）
  const std::string appData = ExeEnvironment(L"APPDATA");
  if (!appData.empty()) {
    const std::string dir = util::JoinPath(appData, "Microsoft\\Windows\\Start Menu\\Programs\\Startup");
    for (const char* name : {"WhalePet.lnk", "鲸鱼桌宠.lnk"}) {
      if (util::FileExists(util::JoinPath(dir, name))) return L"已安装（启动文件夹快捷方式）";
    }
  }
  return L"未设置";
}

/** 读一下 DSH 端现在的桌宠 display（判断会不会出现两只鲸鱼） */
std::wstring DshDisplayState() {
  const std::string url = "http://127.0.0.1:" + std::to_string(g_dshPort) + kRoutePrefix + "/config";
  HttpResponse get = WinHttpRequest("GET", url, {}, "", 1200);
  if (!get.ok) return get.status ? L"HTTP " + std::to_wstring(get.status) : L"DSH 未运行（无法读取）";
  std::string error;
  auto config = js::parse(get.body, &error);
  const js::Value* main = config ? config->get("main") : nullptr;
  const js::Value* pets = main ? main->get("pets") : nullptr;
  if (!pets || !pets->isArr() || pets->arr.empty()) return L"无桌宠配置";
  const js::Value* display = pets->arr[0]->get("display");
  if (!display || !display->isStr()) return L"未知";
  return util::Widen(display->str);
}

int CmdStatus() {
  const bool running = AnotherInstanceRunning();
  const int electron = CountProcesses(L"electron.exe");
  std::wstring text = L"鲸鱼桌宠（独立版 v";
  text += util::Widen(kVersion);
  text += L"）\n\n";
  text += running ? L"宿主：运行中" : L"宿主：未运行";
  text += L"\nElectron 进程数：" + std::to_wstring(electron);
  text += L"\n服务端口：" + std::to_wstring(g_port);
  text += L"\nDSH 端桌宠 display：" + DshDisplayState();
  text += L"\n开机自启：" + AutostartState();
  text += L"\n\n主程序：\n" + util::Widen(util::ExePath());
  text += L"\n\n日志目录：\n" + util::Widen(g_paths.logDir);
  Notify(text);
  return 0;
}

// ---------------------------------------------------------------------------
// 开机自启 / 桌面快捷方式
// ---------------------------------------------------------------------------

std::wstring ExePathW() { return util::Widen(util::ExePath()); }

std::wstring DesktopDir() {
  PWSTR path = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) {
    out = path;
    CoTaskMemFree(path);
  }
  return out;
}

/**
 * 启动入口一律走 start.vbs（而不是直接指向 exe）。
 * 因为 start.vbs 负责「原生存不起就交棒给 Node 宿主」的兜底；快捷方式指向它，
 * 相当于把自动降级能力一起发出去。没有 start.vbs 时才退回直接指向本 exe。
 */
bool LauncherCommand(std::wstring& target, std::wstring& arguments, std::wstring& workDir) {
  const std::string root = util::DirName(g_paths.exeDir);
  const std::string vbs = util::JoinPath(root, "start.vbs");
  if (util::FileExists(vbs)) {
    wchar_t sys[MAX_PATH] = {0};
    if (GetSystemDirectoryW(sys, MAX_PATH)) {
      target = std::wstring(sys) + L"\\wscript.exe";
      arguments = L"\"" + util::Widen(vbs) + L"\"";
      workDir = util::Widen(root);
      return true;
    }
  }
  target = ExePathW();
  arguments.clear();
  workDir = util::Widen(g_paths.exeDir);
  return true;
}

bool InstallShortcut() {
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) return false;
  const std::wstring lnk = desktop + L"\\" + kShortcutName;
  std::wstring target, arguments, workdir;
  LauncherCommand(target, arguments, workdir);

  IShellLinkW* link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                              reinterpret_cast<void**>(&link)))) {
    return false;
  }
  link->SetPath(target.c_str());
  link->SetArguments(arguments.c_str());
  link->SetWorkingDirectory(workdir.c_str());
  link->SetIconLocation(ExePathW().c_str(), 0);
  link->SetDescription(L"鲸鱼桌面宠物（dsh-pet 独立版）");
  IPersistFile* file = nullptr;
  bool ok = false;
  if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
    ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
    file->Release();
  }
  link->Release();
  return ok;
}

bool RemoveShortcut() {
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) return false;
  const std::wstring lnk = desktop + L"\\" + kShortcutName;
  return DeleteFileW(lnk.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool InstallAutostart() {
  std::wstring target, arguments, workDir;
  LauncherCommand(target, arguments, workDir);
  const std::wstring command = L"\"" + target + L"\" " + arguments;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool RemoveAutostart() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;
  const LSTATUS status = RegDeleteValueW(key, kRunValue);
  RegCloseKey(key);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

int CmdInstall() {
  const bool shortcut = InstallShortcut();
  const bool autostart = InstallAutostart();
  std::wstring text;
  text += shortcut ? L"✅ 已创建桌面快捷方式：鲸鱼桌宠" : L"❌ 创建桌面快捷方式失败";
  text += L"\n";
  text += autostart ? L"✅ 已设置开机自启动（登录 Windows 后自动出现桌宠）" : L"❌ 设置开机自启动失败";
  text += L"\n\n主程序：\n" + util::Widen(util::ExePath());
  Notify(text);
  return (shortcut && autostart) ? 0 : 1;
}

int CmdUninstall() {
  const bool shortcut = RemoveShortcut();
  const bool autostart = RemoveAutostart();
  std::wstring text;
  text += shortcut ? L"✅ 已删除桌面快捷方式" : L"❌ 删除桌面快捷方式失败";
  text += L"\n";
  text += autostart ? L"✅ 已移除开机自启动" : L"❌ 移除开机自启动失败";
  Notify(text);
  return 0;
}

// ---------------------------------------------------------------------------
// 配置变化监视
// ---------------------------------------------------------------------------

int64_t ConfigFingerprint() {
  int64_t hash = 1469598103934665603LL;
  auto mix = [&hash](int64_t value) {
    hash ^= value;
    hash *= 1099511628211LL;
  };
  mix(util::FileMtime(g_paths.defaultConfig));
  mix(util::FileMtime(g_paths.userConfig));
  for (const auto& file : PetConfigFiles()) {
    mix(util::FileMtime(file.path));
    mix(static_cast<int64_t>(file.prefix.size()));
  }
  return hash;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

void ParseArgs(int argc, wchar_t** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--smoke") {
      g_smoke = true;
    } else if (arg.rfind(L"--port=", 0) == 0) {
      const int value = _wtoi(arg.c_str() + 7);
      if (value > 0 && value < 65536) g_port = value;
    } else if (arg.rfind(L"--dsh-port=", 0) == 0) {
      const int value = _wtoi(arg.c_str() + 11);
      if (value > 0 && value < 65536) g_dshPort = value;
    } else if (arg == L"--no-fix") {
      g_autoFixDsh = false;
    } else if (arg == L"--ui") {
      g_interactive = true;
    } else if (arg == L"--quiet") {
      g_quiet = true;
      g_interactive = false;
    }
  }
}

int RunHost() {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
  if (!mutex) return Fail("创建单实例互斥量失败。");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    Log("已有独立桌宠在运行，本次启动退出。");
    CloseHandle(mutex);
    return 0;
  }

  g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
  if (!g_stopEvent) {
    CloseHandle(mutex);
    return Fail("创建停止事件失败。");
  }

  Log("============================================================");
  Log(std::string("鲸鱼桌宠 独立宿主（C++）v") + kVersion);
  Log("  DSH_HOME   = " + g_paths.dshHome);
  Log("  包目录     = " + g_paths.packageRoot);
  Log("  Electron   = " + (g_paths.electron.empty() ? std::string("(未找到)") : g_paths.electron));
  Log("  服务端口   = " + std::to_string(g_port));
  Log("  自动纠正DSH = " + std::string(g_autoFixDsh ? "开" : "关"));
  Log("  交互模式   = " + std::string(g_interactive ? "开（弹窗）" : "关（只写日志）"));
  Log("============================================================");

  if (g_paths.electron.empty()) {
    CloseHandle(g_stopEvent);
    CloseHandle(mutex);
    return Fail("找不到 Electron 运行时",
                L"找不到 Electron 运行时。\n\n请先运行一次 DSH 桌宠（会自动下载到 ~\\.dsh\\electron），\n"
                L"或设置环境变量 DSH_PET_ELECTRON_PATH 指向 electron.exe。");
  }
  if (!util::FileExists(g_paths.helperMain)) {
    CloseHandle(g_stopEvent);
    CloseHandle(mutex);
    return Fail("找不到 helper：" + g_paths.helperMain,
                L"找不到 dsh-pet helper 文件。\n\n请确认 DSH 的 dsh-pet 插件已安装。");
  }

  // 端口顺延：和别的宿主（例如 Node 备用宿主）挤在 39471 时不该直接死，
  // 往后找 9 个口，找到哪个报哪个（渲染端配置随之更新，见 ApplyChildEnvironment）。
  // 但先探一下默认口：如果那边已经在应答桌宠接口，说明另一套宿主正跑着——
  // 这时候顺延端口只会得到「桌面上两只鲸鱼」，直接退出才是对的。
  if (!g_smoke) {
    const std::string probeUrl = "http://127.0.0.1:" + std::to_string(kDefaultPort) + kRoutePrefix + "/config";
    HttpResponse probe = WinHttpRequest("GET", probeUrl, {}, "", 700);
    if (probe.ok && probe.status == 200) {
      Log("端口 " + std::to_string(kDefaultPort) + " 上已有桌宠宿主在应答，本次启动退出（避免两只鲸鱼）");
      CloseHandle(g_stopEvent);
      g_stopEvent = nullptr;
      CloseHandle(mutex);
      return 0;
    }
  }

  int bound = 0;
  int attempts = 0;
  for (int i = 0; i < 10; ++i) {
    ++attempts;
    if (g_server.Start(g_port + i)) {
      bound = g_port + i;
      break;
    }
    // WinSock 自己都起不来时换端口毫无意义，立刻收手
    if (g_server.LastError().rfind("socket", 0) == 0) break;
  }
  if (bound == 0) {
    const std::string reason = g_server.LastError();
    CloseHandle(g_stopEvent);
    CloseHandle(mutex);
    return Fail("本地服务端口绑定失败：" + reason + "（已尝试 " + std::to_string(attempts) + " 个端口）");
  }
  if (bound != g_port) Log("端口 " + std::to_string(g_port) + " 不可用，改用 " + std::to_string(bound));
  g_port = bound;
  Log("本地资源服务已监听 http://127.0.0.1:" + std::to_string(g_port) + kRoutePrefix);

  // 先让 DSH 端放弃桌宠（若 DSH 在线），等它把旧 helper 退干净再拉起自己的
  if (FixDshDisplay("启动前")) util::SleepMs(1500);

  g_supervisor.Start(g_smoke ? "smoke" : "initial");

  auto lastFingerprint = std::make_shared<std::atomic<int64_t>>(ConfigFingerprint());
  std::thread([lastFingerprint] {
    for (;;) {
      util::SleepMs(2500);
      if (g_stopping.load()) return;
      const int64_t next = ConfigFingerprint();
      if (next == lastFingerprint->load()) continue;
      lastFingerprint->store(next);
      if (g_smoke.load()) continue;
      g_supervisor.Restart("配置文件变化");
    }
  }).detach();

  std::thread([lastFingerprint] {
    for (;;) {
      util::SleepMs(20000);
      if (g_stopping.load()) return;
      if (FixDshDisplay("周期检查")) lastFingerprint->store(ConfigFingerprint());
    }
  }).detach();

  WaitForSingleObject(g_stopEvent, INFINITE);
  Log("收到停止信号，正在退出…");
  g_stopping = true;
  g_supervisor.Stop();
  g_server.Stop();
  CloseHandle(g_stopEvent);
  g_stopEvent = nullptr;
  CloseHandle(mutex);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDPIAware();
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  InitPaths();
  ParseArgs(argc, argv);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  std::wstring command;
  if (argc > 1) command = argv[1];

  // 只有用户主动敲/双击的命令才允许弹窗；宿主模式（开机自启）和 --smoke 自检永远只写日志。
  const bool isCommand = command == L"--install" || command == L"--uninstall" || command == L"--stop" ||
                         command == L"--status";
  if (isCommand && !g_quiet) g_interactive = true;

  int code = 0;
  if (command == L"--install") {
    code = CmdInstall();
  } else if (command == L"--uninstall") {
    code = CmdUninstall();
  } else if (command == L"--stop") {
    code = CmdStop();
  } else if (command == L"--status") {
    code = CmdStatus();
  } else {
    code = RunHost();
    if (code == 0 && g_exitCode.load() != 0) code = g_exitCode.load();
  }

  if (argv) LocalFree(argv);
  CoUninitialize();
  return code;
}
