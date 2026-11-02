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

TEST(UrmaPriority, RmCtpAdmission) {
    for (unsigned priority : {6u, 7u}) {
        for (unsigned rm_ctp : {0u, 1u}) {
            urma_device_attr_t attr{};
            attr.dev_cap.feature.bs.ctp_en = 1;
            attr.dev_cap.rm_tp_cap.bs.ctp = rm_ctp;
            attr.dev_cap.priority_info[priority].tp_type.bs.ctp = 1;
            const auto result =
                urmaCtpPriorityCapability(attr, priority, URMA_TM_RM);
            EXPECT_TRUE(result.can_attempt_creation) << priority << rm_ctp;
            EXPECT_EQ(result.capability_inconsistent, rm_ctp == 0);
            EXPECT_EQ(result.mode_ctp_supported, rm_ctp != 0);
        }
    }
}

TEST(UrmaPriority, RejectsMissingPriorityOrDeviceCtp) {
    for (unsigned rm_ctp : {0u, 1u}) {
        urma_device_attr_t attr{};
        attr.dev_cap.feature.bs.ctp_en = 1;
        attr.dev_cap.rm_tp_cap.bs.ctp = rm_ctp;
        for (unsigned priority : {6u, 7u}) {
            const auto result =
                urmaCtpPriorityCapability(attr, priority, URMA_TM_RM);
            EXPECT_EQ(result.priority_support, UrmaPrioritySupport::Unknown);
            EXPECT_FALSE(result.can_attempt_creation);
            EXPECT_FALSE(result.capability_inconsistent);
        }
        attr.dev_cap.priority_info[6].tp_type.bs.ctp = 1;
        attr.dev_cap.priority_info[7].tp_type.bs.rtp = 1;
        EXPECT_FALSE(urmaCtpPriorityCapability(attr, 7, URMA_TM_RM)
                         .can_attempt_creation);
        attr.dev_cap.feature.bs.ctp_en = 0;
        EXPECT_FALSE(urmaCtpPriorityCapability(attr, 6, URMA_TM_RM)
                         .can_attempt_creation);
    }
}

TEST(UrmaPriority, RejectsOutOfRangePriority) {
    urma_device_attr_t attr{};
    attr.dev_cap.feature.bs.ctp_en = 1;
    attr.dev_cap.rm_tp_cap.bs.ctp = 1;
    attr.dev_cap.priority_info[6].tp_type.bs.ctp = 1;
    for (unsigned priority : {16u, 256u, static_cast<unsigned>(-1)}) {
        const auto result =
            urmaCtpPriorityCapability(attr, priority, URMA_TM_RM);
        EXPECT_EQ(result.priority_support, UrmaPrioritySupport::Unsupported);
        EXPECT_FALSE(result.can_attempt_creation);
        EXPECT_FALSE(result.capability_inconsistent);
    }
}

TEST(UrmaPriority, RmCompatibilityDoesNotRelaxOtherModes) {
    urma_device_attr_t attr{};
    attr.dev_cap.feature.bs.ctp_en = 1;
    attr.dev_cap.priority_info[7].tp_type.bs.ctp = 1;
    for (auto mode : {URMA_TM_RC, URMA_TM_UM}) {
        auto result = urmaCtpPriorityCapability(attr, 7, mode);
        EXPECT_FALSE(result.can_attempt_creation);
        EXPECT_FALSE(result.capability_inconsistent);
    }
    attr.dev_cap.rc_tp_cap.bs.ctp = 1;
    attr.dev_cap.um_tp_cap.bs.ctp = 1;
    for (auto mode : {URMA_TM_RC, URMA_TM_UM}) {
        EXPECT_TRUE(urmaCtpPriorityCapability(attr, 7, mode)
                        .can_attempt_creation);
        attr.dev_cap.feature.bs.ctp_en = 0;
        EXPECT_FALSE(urmaCtpPriorityCapability(attr, 7, mode)
                         .can_attempt_creation);
        attr.dev_cap.feature.bs.ctp_en = 1;
    }
}
}  // namespace mooncake
