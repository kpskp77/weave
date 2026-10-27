// SVDPI backend VCS driver (reusable across different -sv_lib payloads).
// The same driver is compiled once per target, differing only in the payload library name and its
// work directory (WEAVE_SVDPI_LIB / WEAVE_SVDPI_WORK; the two SV source paths are shared):
//   weave_smoke_svdpi_driver    → libweave_smoke_svdpi (the assertion bodies in tests/)
//   weave_examples_svdpi_driver → libweave_examples_svdpi (the programs in examples/)
// Both run on a real VCS kernel, covering the DPI glue in svdpi.cpp: scope switching,
// fork...join_none spawn scheduling, the three-legged wait (join_any + disable fork), and the
// event table.
//
// Flow:
//   1. locate vcs ($VCS_HOME/bin/vcs or PATH); if not found, print SKIP and exit 0
//      (SVDPI verification is skipped without a VCS/IES environment, see AGENTS.md);
//   2. compile the SVDPI protocol (sv/weave_pkg.sv) and this directory's testbench top
//      (tests/weave_tb.sv) in a dedicated work directory;
//   3. run simv with the payload given by -sv_lib, using the output marker
//      "WEAVE TEST PASSED" as the pass criterion (the $fatal failure path in the tb does not
//      necessarily exit non-zero, so the marker is authoritative).
//
// Synchronisation is fully event-driven (delta cycles); payload contents live in the respective
// directories.

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef WEAVE_SVDPI_PKG_DIR
#error "WEAVE_SVDPI_PKG_DIR must be defined (directory holding weave_pkg.sv)"
#endif
#ifndef WEAVE_SVDPI_TB
#error "WEAVE_SVDPI_TB must be defined (path to the testbench top, tests/weave_tb.sv)"
#endif
#ifndef WEAVE_SVDPI_LIB
#error "WEAVE_SVDPI_LIB must be defined (-sv_lib basename, e.g. libweave_smoke_svdpi)"
#endif
#ifndef WEAVE_SVDPI_WORK
#error "WEAVE_SVDPI_WORK must be defined (per-payload work dir name)"
#endif

namespace {
constexpr char kTag[] = "[" WEAVE_SVDPI_LIB "] ";
constexpr char kLibBase[] = WEAVE_SVDPI_LIB;   // Base name for VCS -sv_lib (no lib/.so)
constexpr char kWorkDir[] = WEAVE_SVDPI_WORK;  // Isolated dir for a fresh compile each run
}  // namespace

namespace {

constexpr int kWatchdogSec = 600;  // VCS compile + simulation well past this counts as hung

void on_alarm(int) {
  const char msg[] = "[svdpi_driver] watchdog timeout, aborting\n";
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
    std::cout << kTag << "vcs not found (VCS_HOME/PATH); SKIP" << std::endl;
    return 0;
  }

  const std::string self = exe_dir();
  const std::string lib = self + "/" + kLibBase + ".so";
  if (access(lib.c_str(), F_OK) != 0) {
    std::cerr << kTag << "missing payload lib: " << lib << std::endl;
    return 2;
  }

  // A fresh compile every run, so a stale simv cannot cause a false positive. All paths are made
  // absolute before switching the working directory (VCS drops simv in the CWD).
  const std::string work = self + "/" + kWorkDir;
  std::error_code ec;
  std::filesystem::remove_all(work, ec);
  std::filesystem::create_directories(work, ec);
  if (::chdir(work.c_str()) != 0) {
    std::cerr << kTag << "chdir(" << work << "): " << std::strerror(errno) << std::endl;
    return 2;
  }

  alarm(kWatchdogSec);

  const std::string pkg = std::string(WEAVE_SVDPI_PKG_DIR) + "/weave_pkg.sv";
  const std::string tb = WEAVE_SVDPI_TB;
  if (run("\"" + vcs + "\" -full64 -sverilog -LDFLAGS \"-Wl,--export-dynamic\" \"" + pkg + "\" \"" +
          tb + "\"") != 0) {
    std::cerr << kTag << "vcs compile failed" << std::endl;
    return 2;
  }

  const std::string log = work + "/simv.log";
  const int simv_rc =
      run("\"./simv\" -sv_root \"" + self + "/\" -sv_lib " + kLibBase + " > \"" + log + "\" 2>&1");

  std::ifstream in(log);
  const std::string output((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::cout << output << std::endl;

  constexpr char kPassMarker[] = "WEAVE TEST PASSED";
  if (simv_rc != 0 || output.find(kPassMarker) == std::string::npos) {
    std::cerr << kTag << "FAILED (simv exit " << simv_rc << ", log: " << log << ")" << std::endl;
    return 1;
  }
  std::cout << kTag << "PASSED" << std::endl;
  return 0;
}
