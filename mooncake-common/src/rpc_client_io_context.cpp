#include "rpc_client_io_context.h"
#include <cstdlib>
#include <string>

namespace mooncake {

namespace {
bool env_flag_enabled(const char* name, bool default_value) {
    const char* env = std::getenv(name);
    if (env == nullptr || *env == '\0') return default_value;
    const std::string v(env);
    return !(v == "0" || v == "false" || v == "False" || v == "FALSE" ||
             v == "off" || v == "no" || v == "No" || v == "NO");
}
}  // namespace

std::shared_ptr<coro_io::io_context_pool> CreateRpcClientIoContextPool(
    uint32_t thread_count) {
    if (const char* list = std::getenv("MC_RPC_CLIENT_CPU_LIST");
        list != nullptr && *list != '\0') {
        setenv("YLT_IO_CONTEXT_CPU_LIST", list, /*overwrite*/ 0);
    }
    const bool cpu_affinity =
        env_flag_enabled("MC_RPC_CLIENT_CPU_AFFINITY", true);
    auto pool =
        std::make_shared<coro_io::io_context_pool>(thread_count, cpu_affinity);
    std::thread thrd{[pool] { pool->run(); }};
    thrd.detach();
    return pool;
}

}  // namespace mooncake
