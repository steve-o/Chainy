// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_status_code.hh"

#include "chromium/logging.hh"

namespace net {

const char* GetHttpReasonPhrase(HttpStatusCode code) {
  switch (code) {

#define HTTP_STATUS(label, code, reason) case HTTP_ ## label: return reason;
#include "net/http/http_status_code_list.hh"
#undef HTTP_STATUS

    default:
      NOTREACHED() << "unknown HTTP status code " << code;
  }

  return "";
}

}  // namespace net
