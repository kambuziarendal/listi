// SPDX-License-Identifier: MIT
#pragma once
#include "DynamicLibrary.h"
#include <curl/curl.h>
#include <mutex>

namespace Listi {
inline DynamicLibrary &Curl() {
  static DynamicLibrary lib({"libcurl.so", "libcurl.so.4"});
  return lib;
}
inline CURL *NewCurlHandle() {
  auto &lib = Curl();
  if (!lib.Available())
    throw std::runtime_error("Install curl from HaikuDepot for FTP and SFTP");
  static std::once_flag initialized;
  std::call_once(initialized, [&] {
    if (LIBCALL(lib, curl_global_init)(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("curl initialization failed");
  });
  auto *handle = LIBCALL(lib, curl_easy_init)();
  if (!handle)
    throw std::runtime_error("Could not allocate network connection");
  return handle;
}
}
