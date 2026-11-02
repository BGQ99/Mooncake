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

#include <gtest/gtest.h>
#include <urma_api.h>
#include <string>
#include "urma_priority.h"

namespace mooncake {
TEST(UrmaPriority, StrictParsing) {
    for (unsigned p = 0; p <= 15; ++p) {
        ASSERT_TRUE(parseUrmaPriority(std::to_string(p)).has_value());
        EXPECT_EQ(*parseUrmaPriority(std::to_string(p)), p);
    }
    for (const char* value : {"", "-1", "16", "256", "99999999999999999999",
                              "7x", " 7", "7 ", "+7"})
        EXPECT_FALSE(parseUrmaPriority(value).has_value()) << value;
}

TEST(UrmaPriority, CtpCapabilityIsNotInferredFromPriorityNumber) {
    urma_device_attr_t attr{};
    EXPECT_EQ(urmaCtpPrioritySupport(attr, 6), UrmaPrioritySupport::Unknown);
    attr.dev_cap.priority_info[6].tp_type.bs.ctp = 1;
    attr.dev_cap.priority_info[7].tp_type.bs.rtp = 1;
    EXPECT_EQ(urmaCtpPrioritySupport(attr, 6), UrmaPrioritySupport::Supported);
    EXPECT_EQ(urmaCtpPrioritySupport(attr, 7), UrmaPrioritySupport::Unsupported);
    EXPECT_EQ(urmaCtpPrioritySupport(attr, 16), UrmaPrioritySupport::Unsupported);
    attr.dev_cap.priority_info[7].tp_type.bs.ctp = 1;
    EXPECT_EQ(urmaCtpPrioritySupport(attr, 7), UrmaPrioritySupport::Supported);
}
}  // namespace mooncake
