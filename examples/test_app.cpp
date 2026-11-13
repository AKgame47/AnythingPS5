// PNR developer-controlled native test application ("the demo game", v0).
// Runs against the PNR compatibility services instead of raw host APIs,
// proving MODULE 03 works for a real compiled x86-64 program.
#include <cstdio>
#include <cstring>

#include "compat/services.hpp"

int main(int argc, char** argv) {
  std::puts("pnr_test_app: native x86-64 demo starting");
  std::printf("pnr_test_app: args=%d\n", argc - 1);

  pnr::compat::MemoryService mem;
  void* block = mem.allocate(4096, 64);
  if (!block) {
    std::puts("pnr_test_app: FAIL memory allocation");
    return 1;
  }
  std::memset(block, 0xA5, 4096);
  std::printf("pnr_test_app: memory ok (%zu bytes outstanding)\n",
              mem.bytes_outstanding());

  pnr::compat::ThreadService threads;
  int shared = 0;
  uint64_t t = threads.spawn([&shared] {
    for (int i = 0; i < 100000; ++i) ++shared;
  });
  (void)t;
  threads.join_all();
  std::printf("pnr_test_app: thread result=%d\n", shared);

  const double t0 = pnr::compat::TimingService::now_seconds();
  pnr::compat::TimingService::sleep_ms(5);
  const double elapsed = pnr::compat::TimingService::now_seconds() - t0;
  std::printf("pnr_test_app: sleep(5ms) measured %.3fms\n", elapsed * 1000.0);

  pnr::compat::FileSystemService fs("./pnr_data");
  pnr::Status st = fs.write_file("/saves/demo_save.txt",
                                 "level=1\nscore=42\nwritten_by=pnr_test_app\n");
  if (!st) {
    std::printf("pnr_test_app: FAIL save write: %s\n", st.error.message.c_str());
    return 1;
  }
  std::string back;
  st = fs.read_file("/saves/demo_save.txt", back);
  if (!st || back.find("score=42") == std::string::npos) {
    std::puts("pnr_test_app: FAIL save readback");
    return 1;
  }
  std::puts("pnr_test_app: save-data write+readback ok");

  mem.deallocate(block);
  std::printf("pnr_test_app: memory released (%zu outstanding)\n",
              mem.bytes_outstanding());
  std::puts("pnr_test_app: ALL OK - exiting 0");
  return 0;
}
