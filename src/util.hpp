// ============================================================================
// util.hpp —— Win32 基础工具：UTF-8/UTF-16、文件读写、目录、时间
//
// 所有对外路径/内容一律用 UTF-8 std::string（与 JSON、HTTP、日志一致），
// 只在调用 Win32 API 的边界处转 UTF-16——中文文件名（素材、字体、用户目录）靠这个保证不乱码。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace util {

inline std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), need);
  return out;
}

inline std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (need <= 0) return std::string();
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), need, nullptr, nullptr);
  return out;
}

inline std::string JoinPath(const std::string& base, const std::string& child) {
  if (base.empty()) return child;
  if (child.empty()) return base;
  std::string out = base;
  const char last = out.back();
  if (last != '\\' && last != '/') out += '\\';
  return out + child;
}

inline std::string DirName(const std::string& path) {
  const size_t pos = path.find_last_of("\\/");
  return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

inline std::string BaseName(const std::string& path) {
  const size_t pos = path.find_last_of("\\/");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

/** 取扩展名（小写，含点；无扩展名返回空） */
inline std::string Extension(const std::string& path) {
  const std::string name = BaseName(path);
  const size_t pos = name.find_last_of('.');
  if (pos == std::string::npos || pos == 0) return std::string();
  std::string ext = name.substr(pos);
  for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return ext;
}

inline bool FileExists(const std::string& path) {
  if (path.empty()) return false;
  const DWORD attr = GetFileAttributesW(Widen(path).c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool DirExists(const std::string& path) {
  if (path.empty()) return false;
  const DWORD attr = GetFileAttributesW(Widen(path).c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/** 文件修改时间（FILETIME 的 64 位值；0 = 不存在） */
inline int64_t FileMtime(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &data)) return 0;
  return (static_cast<int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
         static_cast<int64_t>(data.ftLastWriteTime.dwLowDateTime);
}

inline int64_t FileSize(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &data)) return 0;
  return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | static_cast<int64_t>(data.nFileSizeLow);
}

inline bool ReadFileUtf8(const std::string& path, std::string& out) {
  out.clear();
  HANDLE file = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 30)) {
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    DWORD total = 0;
    while (total < out.size() && ReadFile(file, out.data() + total, static_cast<DWORD>(out.size() - total), &read,
                                          nullptr) && read > 0) {
      total += read;
    }
    out.resize(total);
  } else if (size.QuadPart >= (1LL << 30)) {
    CloseHandle(file);
    return false;  // 配置/素材不可能这么大，直接拒绝
  }
  CloseHandle(file);
  return true;
}

/** 读取整个文件为二进制（素材用；上限放宽到 512MB） */
inline bool ReadFileBinary(const std::string& path, std::string& out, size_t offset = 0, size_t length = SIZE_MAX) {
  out.clear();
  HANDLE file = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1LL << 29)) {
    CloseHandle(file);
    return false;
  }
  const size_t total = static_cast<size_t>(size.QuadPart);
  const size_t begin = offset < total ? offset : total;
  size_t end = total;
  if (length != SIZE_MAX && begin + length < end) end = begin + length;
  const size_t want = end - begin;
  out.resize(want);
  if (want > 0) {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(begin);
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);
    DWORD read = 0;
    size_t got = 0;
    while (got < want && ReadFile(file, out.data() + got, static_cast<DWORD>(want - got), &read, nullptr) && read > 0) {
      got += read;
    }
    out.resize(got);
  }
  CloseHandle(file);
  return true;
}

inline bool WriteFileUtf8(const std::string& path, const std::string& content, bool append = false) {
  HANDLE file = CreateFileW(Widen(path).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  if (append && content.empty()) {
    CloseHandle(file);
    return true;
  }
  DWORD written = 0;
  bool ok = true;
  size_t total = 0;
  while (total < content.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (content.size() - total) > 0x100000 ? 0x100000 : (content.size() - total));
    if (!WriteFile(file, content.data() + total, chunk, &written, nullptr)) {
      ok = false;
      break;
    }
    total += written;
  }
  CloseHandle(file);
  return ok;
}

inline bool EnsureDir(const std::string& dir) {
  if (dir.empty()) return false;
  if (DirExists(dir)) return true;
  // 逐级创建（不依赖 shlobj 的 SHCreateDirectoryEx，保持本头文件只依赖 kernel32）
  const std::wstring wide = Widen(dir);
  std::wstring current;
  for (size_t i = 0; i < wide.size(); ++i) {
    current += wide[i];
    if (wide[i] == L'\\' || wide[i] == L'/' || i + 1 == wide.size()) {
      if (current.size() >= 2 && current.back() == L'\\') continue;  // 跳过盘符后的连续分隔
      CreateDirectoryW(current.c_str(), nullptr);                  // 已存在会返回 ERROR_ALREADY_EXISTS，忽略
    }
  }
  return DirExists(dir);
}

/** 列出目录下的文件名（UTF-8，不含 . 与 ..） */
inline std::vector<std::string> ListDir(const std::string& dir) {
  std::vector<std::string> out;
  std::wstring pattern = Widen(dir);
  if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
  pattern += L'*';
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return out;
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    out.push_back(Narrow(name));
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return out;
}

inline std::string ExePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) return std::string();
    if (len < buffer.size()) {
      buffer.resize(len);
      return Narrow(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
}

inline std::string ExeDir() { return DirName(ExePath()); }

inline std::string NowDate() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
  return buf;
}

inline std::string NowClock() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

inline void SleepMs(int ms) { Sleep(static_cast<DWORD>(ms)); }

}  // namespace util
