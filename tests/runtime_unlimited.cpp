#include "cachescope/TraceABI.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

int main() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "cachescope-unlimited-XXXXXX")
          .string();
  const int fd = mkstemp(pattern.data());
  if (fd < 0)
    return 1;
  close(fd);
  try {
    setenv("CACHESCOPE_TRACE_FILE", pattern.c_str(), 1);
    setenv("CACHESCOPE_TRACE_LIMIT", "unlimited", 1);
    __cachescope_trace_reset_v1();
    __cachescope_register_access_v1(0, 0, 0, 0);
    for (int i = 0; i < 1000001; ++i)
      __cachescope_trace_v1(0, 0, 0, 4, 0);
    __cachescope_trace_flush_v1();
    std::ifstream input(pattern);
    std::string header(2048, '\0');
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    for (const auto *expected :
         {"\"recorded_events\": 1000001", "\"total_events\": 1000001",
          "\"complete\": true", "\"truncated\": false"})
      if (header.find(expected) == std::string::npos)
        throw std::runtime_error("Unlimited runtime truncated the trace");
    std::filesystem::remove(pattern);
    std::cout << "unlimited runtime records beyond one million events\n";
    return 0;
  } catch (const std::exception &error) {
    std::filesystem::remove(pattern);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
