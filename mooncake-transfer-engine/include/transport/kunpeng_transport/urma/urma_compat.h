// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef URMA_COMPAT_H
#define URMA_COMPAT_H

#include <string>
#include <vector>

#include "urma_api.h"

namespace mooncake {

// Runtime compatibility layer for urma_get_rjetty/urma_put_rjetty, which are
// only present in newer UMDK releases. Production symbols are resolved via
// dlsym once per process; mock builds bind their in-process implementation
// directly. When the production symbols are absent, the handshake falls back
// to hand-filled urma_rjetty_t and behavior is identical to the legacy path.
//
// NOTE: do not call urma_get_rjetty/urma_put_rjetty directly anywhere else.
// A direct reference would break linking against older liburma builds.
using urma_get_rjetty_fn_t =
    urma_status_t (*)(urma_jetty_t*, urma_rjetty_t**, uint32_t*);
using urma_put_rjetty_fn_t = void (*)(urma_rjetty_t*);

extern urma_get_rjetty_fn_t g_urma_get_rjetty;
extern urma_put_rjetty_fn_t g_urma_put_rjetty;

void initUrmaRjettyCompat();

bool decodeUrmaRjettyBlob(const std::string& encoded,
                          std::vector<unsigned char>& buffer,
                          std::string& error);

}  // namespace mooncake

#endif  // URMA_COMPAT_H
