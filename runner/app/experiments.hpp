#pragma once

#include <filesystem>
#include <httplib.h>
#include <memory>
#include <string>

namespace cache_simulator {
class Experiments {
public:
  Experiments(httplib::Server &server, const std::filesystem::path &app,
              const std::string &allowed_origin = {});
  ~Experiments();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cache_simulator
