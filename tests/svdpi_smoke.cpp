// SVDPI 后端自检驱动（weave_svdpi_smoke）。
// 与 weave_sc_smoke 共用同一断言体 tests/gather_semantics.cpp（编入
// weave_svdpi_smokelib 共享库），在真实 VCS 内核上回归同一组 asyncio 语义，
// 覆盖 svdpi.cpp 的 DPI 粘合层：scope 切换、fork...join_none 的 spawn 调度、
// 三腿等待（join_any + disable fork）、事件表。
//
// 流程：
//   1. 定位 vcs（$VCS_HOME/bin/vcs 或 PATH）；找不到则打印 SKIP 并以 0 退出
//      （无 VCS/IES 环境跳过 SVDPI 验证，见 AGENTS.md）；
//   2. 在独立工作目录编译 sv/weave_pkg.sv + sv/weave_tb.sv；
//   3. 运行 simv 加载 libweave_svdpi_smokelib.so，以输出标记 "WEAVE TEST PASSED"
//      作为通过判据（tb 中 $fatal 失败路径退出码未必非零，故以标记为准）。
//
// 同步完全事件驱动（delta cycle），断言见 tests/gather_semantics.cpp。

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef WEAVE_SVDPI_SV_DIR
#error "WEAVE_SVDPI_SV_DIR must be defined (path to sv/ sources)"
#endif

namespace {

constexpr int kWatchdogSec = 600;  // VCS 编译 + 仿真远超此值即视为挂死

void on_alarm(int) {
  const char msg[] = "[weave_svdpi_smoke] watchdog timeout, aborting\n";
  write(STDERR_FILENO, msg, sizeof(msg) - 1);
  _exit(2);
}

std::string exe_dir() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  const char* slash = strrchr(buf, '/');
  return slash ? std::string(buf, slash - buf) : std::string();
}

bool is_exe(const std::string& path) {
  return access(path.c_str(), X_OK) == 0;
}

std::string find_vcs() {
  if (const char* home = std::getenv("VCS_HOME")) {
    const std::string cand = std::string(home) + "/bin/vcs";
    if (is_exe(cand)) return cand;
  }
  const char* env_path = std::getenv("PATH");
  if (!env_path) return {};
  const std::string dirs(env_path);
  std::string::size_type pos = 0;
  while (true) {
    const std::string::size_type colon = dirs.find(':', pos);
    const std::string dir =
        dirs.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
    if (!dir.empty() && is_exe(dir + "/vcs")) return dir + "/vcs";
    if (colon == std::string::npos) break;
    pos = colon + 1;
  }
  return {};
}

int run(const std::string& cmd) {
  std::cout << "==> " << cmd << std::endl;
  return std::system(cmd.c_str());
}

}  // namespace

int main() {
  std::signal(SIGALRM, on_alarm);

  const std::string vcs = find_vcs();
  if (vcs.empty()) {
    std::cout << "[weave_svdpi_smoke] vcs not found (VCS_HOME/PATH); SKIP" << std::endl;
    return 0;
  }

  const std::string self = exe_dir();
  const std::string lib = self + "/libweave_svdpi_smokelib.so";
  if (access(lib.c_str(), F_OK) != 0) {
    std::cerr << "[weave_svdpi_smoke] missing smokelib: " << lib << std::endl;
    return 2;
  }

  // 每次全新编译，避免陈旧 simv 造成假阳性。全部路径取绝对路径后切工作目录
  // （VCS 在 CWD 落 simv）。
  const std::string work = self + "/svdpi_smoke_work";
  std::error_code ec;
  std::filesystem::remove_all(work, ec);
  std::filesystem::create_directories(work, ec);
  if (::chdir(work.c_str()) != 0) {
    std::cerr << "[weave_svdpi_smoke] chdir(" << work << "): " << std::strerror(errno) << std::endl;
    return 2;
  }

  alarm(kWatchdogSec);

  const std::string pkg = std::string(WEAVE_SVDPI_SV_DIR) + "/weave_pkg.sv";
  const std::string tb = std::string(WEAVE_SVDPI_SV_DIR) + "/weave_tb.sv";
  if (run("\"" + vcs + "\" -full64 -sverilog -LDFLAGS \"-Wl,--export-dynamic\" \"" + pkg + "\" \"" +
          tb + "\"") != 0) {
    std::cerr << "[weave_svdpi_smoke] vcs compile failed" << std::endl;
    return 2;
  }

  const std::string log = work + "/simv.log";
  const int simv_rc = run("\"./simv\" -sv_root \"" + self +
                          "/\" -sv_lib libweave_svdpi_smokelib > \"" + log + "\" 2>&1");

  std::ifstream in(log);
  const std::string output((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::cout << output << std::endl;

  constexpr char kPassMarker[] = "WEAVE TEST PASSED";
  if (simv_rc != 0 || output.find(kPassMarker) == std::string::npos) {
    std::cerr << "[weave_svdpi_smoke] FAILED (simv exit " << simv_rc << ", log: " << log << ")"
              << std::endl;
    return 1;
  }
  std::cout << "[weave_svdpi_smoke] PASSED" << std::endl;
  return 0;
}
