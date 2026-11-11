#include "runner_api.hpp"
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

using Json = nlohmann::json;

static Json error(const std::string &code, const std::string &message) {
  return {
      {"error",
       {{"code", code}, {"message", message}, {"details", Json::object()}}}};
}

static Json parse(const std::string &body) {
  std::vector<std::set<std::string>> objects;
  return Json::parse(body, [&](int, Json::parse_event_t event, Json &value) {
    if (event == Json::parse_event_t::object_start)
      objects.emplace_back();
    else if (event == Json::parse_event_t::object_end)
      objects.pop_back();
    else if (event == Json::parse_event_t::key &&
             !objects.back().insert(value.get<std::string>()).second)
      throw std::invalid_argument("duplicate JSON object field");
    return true;
  });
}

int main(int argc, char **argv) {
  int port = 9055;
  std::string origin;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--help") {
      std::cout << "usage: cachescope_runner_server [--port 9055] "
                   "[--allow-origin URL]\n";
      return 0;
    }
    if ((option == "--port" || option == "--allow-origin") && i + 1 < argc) {
      const std::string value = argv[++i];
      if (option == "--allow-origin") {
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
  cache_simulator::RunnerApi api;
  httplib::Server server;
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
        auto type = request.get_header_value("Content-Type");
        type = type.substr(0, type.find(';'));
        const auto begin = type.find_first_not_of(" \t");
        type =
            begin == std::string::npos
                ? ""
                : type.substr(begin, type.find_last_not_of(" \t") - begin + 1);
        std::transform(
            type.begin(), type.end(), type.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (type != "application/json")
          throw std::invalid_argument("Content-Type must be application/json");
        body = parse(request.body);
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
  const auto bound = port == 0 ? server.bind_to_any_port("127.0.0.1")
                     : server.bind_to_port("127.0.0.1", port) ? port
                                                              : -1;
  if (bound < 0) {
    std::cerr << "cannot bind HTTP server to 127.0.0.1:" << port << '\n';
    return 1;
  }
  std::cout << "http://127.0.0.1:" << bound << '\n' << std::flush;
  return server.listen_after_bind() ? 0 : 1;
}
