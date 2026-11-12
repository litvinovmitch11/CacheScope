#include "experiments.hpp"
#include "http_json.hpp"
#include "runner_api.hpp"
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <pthread.h>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Json = nlohmann::json;

static Json error(const std::string &code, const std::string &message) {
  return {
      {"error",
       {{"code", code}, {"message", message}, {"details", Json::object()}}}};
}

namespace {
std::filesystem::path make_upload_dir() {
  auto base = std::filesystem::temp_directory_path() / "cachescope-uploads";
  std::filesystem::create_directories(base);
  for (int attempt = 0; attempt < 16; ++attempt) {
    std::error_code ec;
    auto candidate =
        base /
        (std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
         std::to_string(std::rand()) + "-" + std::to_string(attempt));
    if (std::filesystem::create_directory(candidate, ec)) {
      return candidate;
    }
  }
  throw std::runtime_error("cannot create upload directory");
}

std::string sanitize_filename(const std::string &raw) {
  auto name = raw;
  const auto slash = name.find_last_of("/\\");
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  if (name.empty() || name == "." || name == "..") {
    throw std::invalid_argument("invalid filename");
  }
  for (unsigned char c : name) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':') {
      throw std::invalid_argument("invalid character in filename");
    }
  }
  return name;
}
} // namespace

int main(int argc, char **argv) {
  int port = 9055;
  std::string origin, host = "127.0.0.1";
  std::filesystem::path app;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--help") {
      std::cout << "usage: cachescope_runner_server [--port 9055] "
                   "[--allow-origin URL] [--host IP] [--app-dir ROOT]\n";
      return 0;
    }
    if ((option == "--port" || option == "--allow-origin" ||
         option == "--host" || option == "--app-dir") &&
        i + 1 < argc) {
      const std::string value = argv[++i];
      if (option == "--host") {
        if (value != "127.0.0.1" && value != "0.0.0.0") {
          std::cerr << "host must be 127.0.0.1 or 0.0.0.0\n";
          return 2;
        }
        host = value;
      } else if (option == "--app-dir") {
        app = std::filesystem::absolute(value);
      } else if (option == "--allow-origin") {
        if (value.empty() || value.find_first_of("\r\n") != std::string::npos) {
          std::cerr << "invalid origin\n";
          return 2;
        }
        origin = value;
      } else {
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), port);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || port < 0 ||
            port > 65535) {
          std::cerr << "invalid port\n";
          return 2;
        }
      }
    } else {
      std::cerr << "unknown or incomplete option: " << option << '\n';
      return 2;
    }
  }
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  cache_simulator::RunnerApi api;
  httplib::Server server;
  cache_simulator::Experiments experiments(server, app, origin);
  if (!app.empty()) {
    server.set_payload_max_length(std::numeric_limits<std::size_t>::max());
    if (!server.set_mount_point("/", (app / "ui/dist").string())) {
      std::cerr << "UI build missing; build ui/dist before starting the app\n";
      return 1;
    }
  }
  server.set_pre_routing_handler(
      [](const httplib::Request &request, httplib::Response &response) {
        auto host = request.get_header_value("Host");
        host = host.substr(0, host.find(':'));
        for (auto &c : host)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (host != "127.0.0.1" && host != "localhost") {
          response.status = 400;
          response.set_content(
              error("invalid_request", "Host must be loopback").dump(),
              "application/json");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });
  const auto handler = [&](const httplib::Request &request,
                           httplib::Response &response) {
    Json body;
    if (request.method == "POST") {
      try {
        if (!cache_simulator::json_content_type(
                request.get_header_value("Content-Type")))
          throw std::invalid_argument("Content-Type must be application/json");
        body = cache_simulator::parse_api_json(request.body);
      } catch (const std::exception &exception) {
        response.status = 400;
        response.set_content(error("invalid_request", exception.what()).dump(),
                             "application/json");
        return;
      }
    }
    std::multimap<std::string, std::string> query;
    for (const auto &[key, value] : request.params)
      query.emplace(key, value);
    auto result = api.handle(request.method, request.path, query, body);
    response.status = result.status;
    if (!result.location.empty())
      response.set_header("Location", result.location);
    if (result.status != 204)
      response.set_content(result.body.dump(), "application/json");
  };

  server.Post("/api/v1/uploads", [](const httplib::Request &req,
                                    httplib::Response &res) {
    try {
      // Проверяем, что это действительно multipart/form-data
      if (!req.is_multipart_form_data()) {
        throw std::invalid_argument("Content-Type must be multipart/form-data");
      }

      // В новом API cpp-httplib файлы доступны через req.form
      if (!req.form.has_file("trace")) {
        throw std::invalid_argument("missing 'trace' file");
      }

      std::filesystem::path dir = make_upload_dir();
      std::filesystem::path trace_path = dir / "trace.json";
      std::vector<std::filesystem::path> stage_paths;

      // 1. Сохраняем обязательный trace.json
      const auto &trace_file = req.form.get_file("trace");
      std::ofstream(trace_path, std::ios::binary)
          .write(trace_file.content.data(),
                 static_cast<std::streamsize>(trace_file.content.size()));

      // 2. Сохраняем опциональные MLIR-файлы (поле "stages")
      // get_files возвращает все файлы, отправленные под одним именем поля
      for (const auto &stage_file : req.form.get_files("stages")) {
        if (stage_file.filename.empty())
          continue;
        auto stage_path = dir / sanitize_filename(stage_file.filename);
        std::ofstream(stage_path, std::ios::binary)
            .write(stage_file.content.data(),
                   static_cast<std::streamsize>(stage_file.content.size()));
        stage_paths.push_back(std::move(stage_path));
      }

      Json stage_array = Json::array();
      for (const auto &p : stage_paths)
        stage_array.push_back(p.string());

      res.status = 200;
      res.set_content(Json{{"trace_path", trace_path.string()},
                           {"stage_paths", std::move(stage_array)}}
                          .dump(),
                      "application/json");
    } catch (const std::exception &e) {
      res.status = 400;
      res.set_content(error("invalid_request", e.what()).dump(),
                      "application/json");
    }
  });

  server.Get("/api/v1/source", [](const httplib::Request &req,
                                  httplib::Response &res) {
    try {
      if (!req.has_param("trace_path") || !req.has_param("stage_path")) {
        throw std::invalid_argument("need trace_path and stage_path");
      }

      const auto trace_path =
          std::filesystem::absolute(req.get_param_value("trace_path"));
      const auto trace_dir = trace_path.parent_path();
      const auto target = std::filesystem::weakly_canonical(
          trace_dir / req.get_param_value("stage_path"));

      const auto rel = std::filesystem::relative(target, trace_dir);
      if (rel.empty() || *rel.begin() == "..") {
        throw std::invalid_argument("stage_path escapes trace directory");
      }
      if (!std::filesystem::is_regular_file(target)) {
        throw std::invalid_argument("stage file not found");
      }

      std::ifstream in(target, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());

      res.status = 200;
      res.set_content(Json{{"text", std::move(text)}}.dump(),
                      "application/json");
    } catch (const std::exception &e) {
      res.status = 400;
      res.set_content(error("invalid_request", e.what()).dump(),
                      "application/json");
    }
  });

  server.Get(R"(/api/v1/.*)", handler);
  server.Post(R"(/api/v1/.*)", handler);
  server.Delete(R"(/api/v1/.*)", handler);
  server.Options(R"(/api/v1/.*)",
                 [](const httplib::Request &, httplib::Response &response) {
                   response.status = 204;
                 });
  server.set_post_routing_handler(
      [&](const httplib::Request &request, httplib::Response &response) {
        if (!origin.empty() && request.get_header_value("Origin") == origin) {
          response.set_header("Access-Control-Allow-Origin", origin);
          response.set_header("Vary", "Origin");
          response.set_header("Access-Control-Allow-Methods",
                              "GET, POST, DELETE, OPTIONS");
          response.set_header("Access-Control-Allow-Headers", "Content-Type");
          response.set_header("Access-Control-Expose-Headers", "Location");
        }
        response.set_header("Cache-Control", "no-store");
      });
  server.set_error_handler(
      [](const httplib::Request &,
         httplib::Response &response) -> httplib::Server::HandlerResponse {
        if (response.get_header_value("Content-Type") != "application/json") {
          if (response.status != 404 && response.status < 500)
            response.status = 400;
          response.set_content(error(response.status == 404
                                         ? "session_not_found"
                                     : response.status < 500 ? "invalid_request"
                                                             : "internal_error",
                                     "HTTP request failed")
                                   .dump(),
                               "application/json");
        }
        return httplib::Server::HandlerResponse::Handled;
      });
  server.set_exception_handler([](const httplib::Request &,
                                  httplib::Response &response,
                                  std::exception_ptr exception) {
    response.status = 500;
    try {
      std::rethrow_exception(std::move(exception));
    } catch (const std::exception &failure) {
      response.set_content(error("internal_error", failure.what()).dump(),
                           "application/json");
    } catch (...) {
      response.set_content(
          error("internal_error", "unexpected HTTP failure").dump(),
          "application/json");
    }
  });
  const auto bound = port == 0 ? server.bind_to_any_port(host)
                     : server.bind_to_port(host, port) ? port
                                                       : -1;
  if (bound < 0) {
    std::cerr << "cannot bind HTTP server to 127.0.0.1:" << port << '\n';
    return 1;
  }
  std::cout << "http://127.0.0.1:" << bound << '\n' << std::flush;
  std::thread shutdown([&] {
    int signal = 0;
    sigwait(&signals, &signal);
    server.stop();
  });
  const bool success = server.listen_after_bind();
  pthread_kill(shutdown.native_handle(), SIGTERM);
  shutdown.join();
  return success ? 0 : 1;
}
