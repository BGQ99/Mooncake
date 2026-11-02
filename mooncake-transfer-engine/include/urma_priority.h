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

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mooncake {
inline std::optional<uint8_t> parseUrmaPriority(std::string_view input) {
    unsigned priority = 0;
    auto result = std::from_chars(input.data(), input.data() + input.size(),
                                  priority);
    if (result.ec != std::errc{} ||
        result.ptr != input.data() + input.size() || priority > 15)
        return std::nullopt;
    return static_cast<uint8_t>(priority);
}

enum class UrmaPrioritySupport { Unknown, Unsupported, Supported };

// priority_info describes TP types, not per-mode (RM/RC) QoS guarantees.
template <typename DeviceAttr>
UrmaPrioritySupport urmaCtpPrioritySupport(const DeviceAttr& attr,
                                         unsigned priority) {
    if (priority > 15)
        return UrmaPrioritySupport::Unsupported;
    bool reported = false;
    for (const auto& info : attr.dev_cap.priority_info)
        reported |= info.tp_type.value != 0;
    if (!reported)
        return UrmaPrioritySupport::Unknown;
    return attr.dev_cap.priority_info[priority].tp_type.bs.ctp
               ? UrmaPrioritySupport::Supported
               : UrmaPrioritySupport::Unsupported;
}
}  // namespace mooncake
