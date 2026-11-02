// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "http-crawler.h"
#include <iostream>

static cardio::promise<void> run_async(
    int argc, char **argv, cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::CurlHttpSession> session;
  try {
    using namespace elder_terms;
    const auto scope = http_scope_url(argv[1]);
    CurlHttpSettings settings;
    settings.scheme = scope.starts_with("https:") ? "https" : "http";
    settings.address = "127.0.0.1";
    settings.authentication = HttpAuthentication::none;
    session = create_curl_http_session(settings, "");
    for (int index = 2; index < argc; ++index) {
      const auto url = http_resolve_url(scope, argv[index]);
      const auto page = co_await fetch_http_page_async(session, scope, url, false, {}, {});
      std::cout << (page.html ? "html" : "content") << '\t' << page.resource.path << '\n';
      for (const auto &entry : page.directory.entries)
        std::cout << "link\t" << entry.path << '\t' << entry.name << '\n';
    }
  } catch (...) { failure = std::current_exception(); }
  if (session) co_await elder_terms::stop_curl_http_session_async(session);
  group.shutdown();
}

int main(int argc, char **argv) {
  try {
    if (argc < 3) throw std::invalid_argument("Expected scope URL and pages to inspect");
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto pending = run_async(argc, argv, group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
