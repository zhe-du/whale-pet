// probe.cpp —— 最小 CreateProcess 探针
// 目的：把「宿主用 CreateProcessW 拉起 Electron 立刻 0x80000003 退出」这一现象
//       拆成可单独复现的最小用例，并逐项对比不同启动参数。
// 用法：probe.exe <electron.exe> <main.js> [mode] [cwd]
//   mode: simple | pipe | file | console
// 输出：控制台 + probe.log（同目录），两者都写，GUI/控制台子系统都能拿到结果。
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>

static FILE* g_log = nullptr;

static void Out(const wchar_t* fmt, ...) {
  wchar_t buf[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf(buf, 2047, fmt, ap);
  va_end(ap);
  buf[2047] = 0;
  if (g_log) {
    // 落盘用 UTF-8
    char utf8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (n > 0) {
      utf8[n] = 0;
      fputs(utf8, g_log);
      fflush(g_log);
    }
  }
  fwprintf(stdout, L"%ls\n", buf);
  fflush(stdout);
}

static BOOL TryLaunch(const std::wstring& electron, const std::wstring& helper, const std::wstring& mode,
                      const std::wstring& cwd) {
  SetEnvironmentVariableW(L"DSH_PET_CONFIG_URL", L"http://127.0.0.1:39471/dsh-pet-7340/config");
  SetEnvironmentVariableW(L"DSH_PET_PETS", L"[{\"id\":\"main\",\"size\":440}]");
  SetEnvironmentVariableW(L"DSH_PET_SCALE", L"1");
  SetEnvironmentVariableW(L"ELECTRON_RUN_AS_NODE", nullptr);
  SetEnvironmentVariableW(L"DSH_PET_BRIDGE", nullptr);
  SetEnvironmentVariableW(L"DSH_PET_HOST_PID", nullptr);

  HANDLE outWrite = INVALID_HANDLE_VALUE, errWrite = INVALID_HANDLE_VALUE;
  HANDLE outRead = INVALID_HANDLE_VALUE, errRead = INVALID_HANDLE_VALUE;

  std::wstring cmd = L"\"" + electron + L"\" \"" + helper + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  DWORD flags = 0;

  if (mode == L"pipe") {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    CreatePipe(&outRead, &outWrite, &sa, 0);
    CreatePipe(&errRead, &errWrite, &sa, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    flags = CREATE_NO_WINDOW;
    Out(L"[probe] mode=pipe");
  } else if (mode == L"file") {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(L"probe-helper.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    outWrite = log;
    Out(L"[probe] mode=file");
  } else if (mode == L"console") {
    // 不给任何 stdio 覆盖，也不设 CREATE_NO_WINDOW：子进程共享控制台
    Out(L"[probe] mode=console (no STARTF_USESTDHANDLES)");
  } else {
    Out(L"[probe] mode=simple (inherit stdio)");
  }

  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                 &si, &pi);
  if (outWrite != INVALID_HANDLE_VALUE) CloseHandle(outWrite);
  if (errWrite != INVALID_HANDLE_VALUE) CloseHandle(errWrite);
  if (!ok) {
    Out(L"[probe] CreateProcess failed: %lu", GetLastError());
    return FALSE;
  }

  const DWORD wr = WaitForSingleObject(pi.hProcess, 6000);
  if (wr == WAIT_TIMEOUT) {
    Out(L"[probe] RESULT: ALIVE (pid=%lu) -> 启动方式没问题，进程活过 6s", pi.dwProcessId);
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
  } else {
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    Out(L"[probe] RESULT: EXITED code=0x%08lX (%lu)", code, code);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (outRead != INVALID_HANDLE_VALUE) CloseHandle(outRead);
  if (errRead != INVALID_HANDLE_VALUE) CloseHandle(errRead);
  return TRUE;
}

int wmain(int argc, wchar_t** argv) {
  g_log = _wfopen(L"probe.log", L"a, ccs=UTF-8");
  if (argc < 3) {
    Out(L"usage: probe <electron.exe> <main.js> [simple|pipe|file|console] [cwd]");
    return 1;
  }
  const std::wstring mode = argc > 3 ? argv[3] : L"simple";
  const std::wstring cwd = argc > 4 ? argv[4] : L"";
  Out(L"----- probe %ls @ %lu -----", mode.c_str(), GetCurrentProcessId());
  TryLaunch(argv[1], argv[2], mode, cwd);
  if (g_log) fclose(g_log);
  return 0;
}
