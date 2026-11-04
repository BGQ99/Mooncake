# Mooncake 930 分支编译部署 + 1-1 Get Benchmark 测试流程

> 目标：把 `nof-gdr-supercache-snapshot-930` 分支（commit `706551a8`）编译部署到集群节点，
> 然后用 `stress_cluster_bench` 跑 NoF 1-1 Get 性能测试（单 source / 多并发组合）。
>
> - 代码仓库：`https://github.com/LeeJace993/Mooncake.git`，分支 `nof-gdr-supercache-snapshot-930`
> - HEAD commit：`706551a8`（`docs: add nof-gdr-930 build/deploy + JD benchmark runbook`）
> - SPDK：pin `e12b7cb213a9c1a2a22b99ab4732c8c11f95032c`（urma_enable_v2，wire v4 遥测 72B）
> - 关联文档：skill `mooncake-nof-bench`（集群拓扑 / 环境变量 / 历史基线）
>
> 集群拓扑：node1 141.61.84.245 / node2 141.61.84.247 / node3 141.61.84.149 /
> node4 141.61.84.151，任意节点可任角色。
>
> SSH（本机 Git bash → ssh.exe）：
> ```bash
> "/c/Program Files (x86)/Git/bin/ssh.exe" \
>   -i /c/Users/y00988740/.ssh/urma_diag_key \
>   -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
>   root@<node-ip> "<command>"
> ```

---

## 0. 进程总账（先读这节）

| 编号 | 进程 | 节点 | 启动方式 | 生命周期 | 存活确认 |
|---|---|---|---|---|---|
| P0 | `nvmf_tgt`（NoF target） | target | `nvmf_tgt -m 0xfc --wait-for-rpc -r <sock>` + RPC（§3.1） | 常驻；**target 重启后重来** | `ss -tlnp \| grep 4420` |
| P1 | `http_metadata_server` | store/reader | `python -m mooncake.http_metadata_server --port 8080` | 常驻 | `curl http://127.0.0.1:8080/metadata` |
| P2 | `mooncake_master` | store/reader | `mooncake_master --rpc_port=50051 --metrics_port=9003 ...` | 常驻 | `ss -tlnp \| grep 50051` |
| P3 | NoF SSD 注册（一次性） | reader | `nof_register <nqn> <nsid> <traddr> <trsvcid> <trtype> <master_addr> [size_bytes]` | 跑一次即退 | master 日志/`:9003` 看到 SSD |
| P4 | `mooncake_client`（store 实例） | store | `mooncake_client --host <IP> ... --device_names udmac0d1e2` | 常驻 | 日志 "UbTransport init done" |
| P5 | `stress_cluster_bench` | reader | `./stress_cluster_bench --scenario=segment_read ...` | 按 duration 退出 | 日志 "Throughput (avg)" |

⚠️ **master 重启后 NoF segment 丢失**（segment 挂在 master 内存里），P3 必须重跑；
target 重启则 P0 全套（vfio 绑定 + RPC + P3）重来。

---

## 1. 编译部署

### 1.1 前提：SPDK e12b7cb21（两节点都要）

SPDK 已预装在两节点的 `/home/yin/nof/spdk` 和 `/usr/local/bin/nvmf_tgt`（`make install`）。
如需重编：

```bash
# 在节点上
cd /home/yin/nof/spdk
git log --oneline -1        # 必须 e12b7cb213a9c1a2a22b99ab4732c8c11f95032c
./configure --with-rdma --with-urma=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk \
            --disable-unit-tests   # e12b7cb21+ 需加此 flag 避开 spdk_get_ticks 链接回归
make -j$(nproc) && make install
# 验收
ls /usr/local/include/spdk/nvme_urma.h
ls /usr/local/lib/libspdk_nvme.a
```

⚠️ **两端 SPDK 必须同一 commit**（wire v4 遥测 72B，v3/v4 不能混用）。

### 1.2 克隆 930 分支

```bash
# 在节点上
cd /home/yin/nof
git clone --branch nof-gdr-supercache-snapshot-930 \
          --recurse-submodules \
          https://github.com/LeeJace993/Mooncake.git \
          mooncake-930
cd mooncake-930
git log --oneline -1        # 必须 706551a8
```

⚠️ **node2 代理陷阱**：node2 的 `~/.gitconfig` 配了坏代理 `141.1.139.187:8989`（不可达）。
clone 前必须覆盖：

```bash
export http_proxy=http://127.0.0.1:3128 https_proxy=http://127.0.0.1:3128
export GIT_CONFIG_COUNT=2 \
       GIT_CONFIG_KEY_0=http.proxy GIT_CONFIG_VALUE_0=http://127.0.0.1:3128 \
       GIT_CONFIG_KEY_1=https.proxy GIT_CONFIG_VALUE_1=http://127.0.0.1:3128
# 然后再 git clone
```

### 1.3 Python venv + 依赖

```bash
cd /home/yin/nof/mooncake-930
python3 -m venv ~/venvs/mooncake-bench
source ~/venvs/mooncake-bench/bin/activate
pip install -U pip && pip install aiohttp paramiko   # metadata server / 注册脚本运行期依赖
```

### 1.4 修复源码 bug：client_service.cpp:3046

⚠️ **930 分支自带一个编译错误**：`client_service.cpp` 第 3046 行调用了
`start_result.value().size()`，但 `PutStartResult` 结构体（定义在 `rpc_types.h:98`）
没有 `size()` 方法，只有 `replicas`（vector）。需手动修复：

```bash
cd /home/yin/nof/mooncake-930
# 修复前先备份
cp mooncake-store/src/client_service.cpp mooncake-store/src/client_service.cpp.bak_706551a8
# 将 .size() 改为 .replicas.size()
sed -i '3046s/start_result.value().size()/start_result.value().replicas.size()/' \
    mooncake-store/src/client_service.cpp
# 验证
sed -n '3046p' mooncake-store/src/client_service.cpp
# 应输出： << " replicas=" << start_result.value().replicas.size()
```

### 1.5 cmake + make

两节点的 UMDK 头文件路径不同，cmake 参数有差异：

#### node4（UMDK_tool 头文件完整）

```bash
cd /home/yin/nof/mooncake-930
source ~/venvs/mooncake-bench/bin/activate
mkdir -p build && cd build
cmake .. -DUSE_NOF=ON \
         -DUSE_NOF_URMA=ON \
         -DUSE_UB=ON \
         -DCMAKE_BUILD_TYPE=Release \
         -DUMDK_ROOT=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk \
         -DURMA_LIBRARY=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so
make -j$(nproc)
make install
```

#### node2（UMDK_tool/include/ 为空，用系统头）

```bash
cd /home/yin/nof/mooncake-930
source ~/venvs/mooncake-bench/bin/activate
mkdir -p build && cd build
# node2 的 UMDK_tool_netlab-sp4_umdk/include/ 不存在，必须用 URMA_ROOT 指向系统头
cmake .. -DUSE_NOF=ON \
         -DUSE_NOF_URMA=ON \
         -DUSE_UB=ON \
         -DCMAKE_BUILD_TYPE=Release \
         -DURMA_ROOT=/usr/include/ub/umdk/urma \
         -DURMA_LIBRARY=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so
make -j$(nproc)
make install
```

⚠️ **`-DURMA_LIBRARY` 必须显式指定**：系统 `/usr/lib64/liburma.so`（旧版）缺少
`urma_register_seg_dmabuf` 符号，链接会报 `undefined reference`。
`/home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so` 有此符号，必须显式传入。

⚠️ **node2 FetchContent 代理**：cmake 的 FindUrma.cmake 和 spdiag FetchContent
会从 `atomgit.com` / `gitcode.com` clone。node2 坏代理下会失败，cmake 前设好
§1.2 的代理环境变量。

### 1.6 验收

```bash
# 二进制存在
ls -la build/mooncake-store/src/mooncake_master      # ~8.5 MB
ls -la build/mooncake-store/src/mooncake_client      # ~15 MB
ls -la build/mooncake-store/src/nof_register          # ~171 MB
ls -la build/mooncake-store/benchmarks/stress_cluster_bench  # ~465 MB

# make install 到 /usr/local
ls -la /usr/local/bin/mooncake_master /usr/local/bin/mooncake_client

# store.so 导入
python -c "from mooncake.store import MooncakeDistributedNoFRegister; print('store.so OK')"

# nof_register 用法
./build/mooncake-store/src/nof_register 2>&1 | head -1
# Usage: nof_register <nqn> <nsid> <traddr> <trsvcid> <trtype> <master_addr> [size_bytes]

# liburma 符号
nm -D /home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so | grep urma_register_seg_dmabuf
# 应输出：0000000000027150 T urma_register_seg_dmabuf
```

---

## 2. 运行期环境变量

所有 930 进程（nvmf_tgt、master、client、bench）启动前必须设置：

```bash
export SPDK_URMA_DEV_NAME=udmac0d1e2          # URMA 设备名（本环境 bonding_dev_0 不可用）
export SPDK_URMA_CAPSULE_TRANSPORT=urma       # URMA capsule 传输
export MC_HIFREQ_LOG_SAMPLE_RATE=0            # 关闭高频日志采样（默认 1.0 全采样刷屏！）
export MC_SLICE_SIZE=1048576                  # 1MB 切片
export MC_STORE_MEMCPY=1                      # 启用 local_memory memcpy 优化
export MC_STORE_MEMCPY_INLINE=1               # inline 执行 host memcpy
export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib
```

⚠️ **`MC_HIFREQ_LOG_SAMPLE_RATE` 默认 1.0 陷阱**：未设时默认全采样，不是 0！
所有进程启动必须显式设 `=0`，否则日志刷屏污染性能测量。采样率是 `static const`
（`mooncake_logging.cpp:309`），进程启动时一次性解析，运行中无法改。

### NoF 性能调优变量（按需）

| 变量 | 推荐值 | 说明 |
|------|--------|------|
| `MC_NOF_WORKERS` | 4 | NoF worker 线程数（≥ qpairs） |
| `MC_NOF_QPAIRS_PER_SEGMENT` | 4 | 每 segment 的 qpair lane 数（4qp 突破到 ~47 GB/s） |
| `MC_NOF_INFLIGHT_BYTES_LIMIT` | 50331648 (48M) | segment 总预算（代码自动 ÷ lane_count） |
| `MC_NOF_SUBMIT_CHUNK_BYTES` | 131072 (128K) | 单次 I/O chunk 大小 |
| `MC_NVME_IO_QUEUE_SIZE` | 256 | 单 qpair 队列深度 |
| `MC_NVME_IO_QUEUE_REQUESTS` | 512 | 单 qpair request 数 |
| `MC_NOF_WHOLE_POOL_REGISTER` | 1 | 整池注册开关 |

---

## 3. 部署启动

### 3.1 target 节点：NoF target（P0）

```bash
# target 节点（如 node4）
export SPDK_URMA_DEV_NAME=udmac0d1e2
export SPDK_URMA_CAPSULE_TRANSPORT=urma
export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib

# 1. vfio-pci 绑定 SSD（以单盘 nvme0 / BDF a4:00.0 为例）
modprobe vfio-pci
echo "vfio-pci" > /sys/bus/pci/devices/0000:a4:00.0/driver_override
echo "0000:a4:00.0" > /sys/bus/pci/drivers/nvme/unbind
echo "0000:a4:00.0" > /sys/bus/pci/drivers_probe

# 2. 启动 nvmf_tgt（CPU 亲和 mask 0xfc = cores 2-7，避开 CPU 0/1）
setsid nohup nvmf_tgt -m 0xfc --wait-for-rpc -r /var/tmp/spdk.sock \
    > /tmp/nvmf_tgt.log 2>&1 < /dev/null & disown

# 3. SPDK RPC 序列
scripts/rpc.py -s /var/tmp/spdk.sock rpc_get_methods   # 确认 tgt 就绪
scripts/rpc.py -s /var/tmp/spdk.sock iobuf_set_options \
    small_pool_count=12288 small_bufsize=8192 large_pool_count=512 large_bufsize=4194304
scripts/rpc.py -s /var/tmp/spdk.sock framework_start_init

# 4. 创建 bdev（单盘）
scripts/rpc.py -s /var/tmp/spdk.sock bdev_nvme_attach_controller \
    -b Nvme0 -t PCIe -a 0000:a4:00.0
# 多盘 RAID0（如 11 盘）：
# scripts/rpc.py -s /var/tmp/spdk.sock bdev_raid_create \
#     -b R11_RAID0 -r raid0 -s 256 \
#     -z "Nvme0n1 Nvme1n1 Nvme2n1 ... Nvme10n1"

# 5. 创建 NoF transport
scripts/rpc.py -s /var/tmp/spdk.sock nvmf_create_transport \
    -t URMA -c urma --max-io-size 4194304 \
    --iobuf-large-cache-size 32 --iobuf-small-cache-size 1024 \
    --dev-name udmac0d1e2 --eid-index 0

# 6. 创建 subsystem + namespace + listener
NQN=nqn.2026-01.io.spdk:mooncake-jd
scripts/rpc.py -s /var/tmp/spdk.sock nvmf_create_subsystem "$NQN" -a -s JD0001
scripts/rpc.py -s /var/tmp/spdk.sock nvmf_subsystem_add_ns "$NQN" Nvme0n1
scripts/rpc.py -s /var/tmp/spdk.sock nvmf_subsystem_add_listener "$NQN" \
    -t URMA -a 141.61.84.151 -s 4420 -f IPv4
```

⚠️ **listener 必须加 `-f IPv4`**：不加则 adrfam=unknown，host 连接被拒
（`Subsystem does not allow host ... to connect at this address`）。

⚠️ **listener 必须用精确 IP**，不能 `0.0.0.0`。

### 3.2 store 节点：master 三件套（P1/P2/P4）

如果 target 节点同时做 store（单节点部署），在同一节点启动：

```bash
export SPDK_URMA_DEV_NAME=udmac0d1e2
export SPDK_URMA_CAPSULE_TRANSPORT=urma
export MC_HIFREQ_LOG_SAMPLE_RATE=0
export MC_SLICE_SIZE=1048576
export MC_STORE_MEMCPY=1 MC_STORE_MEMCPY_INLINE=1
export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib
source ~/venvs/mooncake-bench/bin/activate

# P1: metadata server
setsid nohup python -m mooncake.http_metadata_server --port 8080 \
    > /tmp/metadata.log 2>&1 < /dev/null & disown

# P2: master（930 用 --rpc_port，不是旧文档的 --port）
setsid nohup mooncake_master \
    --rpc_port=50051 \
    --metrics_port=9003 \
    --enable_http_metadata_server \
    --http_metadata_server_port=8080 \
    --rpc_address=141.61.84.151 \
    --rpc_thread_num=32 \
    --eviction_high_watermark_ratio=0.9 \
    --default_kv_lease_ttl=3600000 \
    --nof_heartbeat_failures_threshold=999999 \
    --nof_heartbeat_interval_sec=3600 \
    > /tmp/master.log 2>&1 < /dev/null & disown

# P4: store client（内存段载体；--device_names 必须指定 udmac0d1e2）
setsid nohup mooncake_client \
    --host 141.61.84.151 \
    --metadata_server http://141.61.84.151:8080/metadata \
    --master_server_address 141.61.84.151:50051 \
    --protocol ub --device_names udmac0d1e2 \
    --global_segment_size 4GB --threads 4 \
    > /tmp/client.log 2>&1 < /dev/null & disown
```

⚠️ **mooncake_client 必须加 `--device_names udmac0d1e2`**：默认 `bonding_dev_0`
不可用（`urma_create_device` 失败），会报 `No available RNIC` 并 fallback 到
`mock_urma_device`。

### 3.3 NoF SSD 注册（P3，一次性）

```bash
# 在 reader/store 节点执行（与 master 同节点）
NQN=nqn.2026-01.io.spdk:mooncake-jd
export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib

# C++ 直注工具（930 新增，免 SSH 读 RPC，适合脚本化）
nof_register "$NQN" 1 141.61.84.151 4420 URMA 141.61.84.151:50051 10737418240
# 参数：<nqn> <nsid> <traddr> <trsvcid> <trtype> <master_addr> [size_bytes]
# size_bytes 必须 > 0，否则 segment.cpp:1361 拒绝（"buffer=0 or size=0 is invalid"）
```

验收：master `:9003` metrics / 日志出现注册的 SSD；`curl http://<IP>:9003` 可见
NoF segment capacity。

---

## 4. 冒烟测试

部署完成后，按以下顺序验证通路。

### 4.1 通路级：SPDK endpoint 直测（store 节点）

```bash
export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib
EP="trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd ns:1"

# ① 对齐单测
./build/mooncake-store/tests/nof_buffer_alignment_test

# ② host 内存 read 基线
./build/mooncake-store/benchmarks/nof_worker_pool_bench \
    --endpoints "$EP" --op=read --io_size=128K --iodepth=64 \
    --duration_sec=20 --range_bytes=1G
```

通过标准：①全部 PASS；②IOPS/带宽稳定输出、errors=0。
实测参考（node4 单 SSD）：11.3 GiB/s，92.6K IOPS，fail_iops=0。

### 4.2 两节点 1-1 Get 冒烟（跨节点）

在 reader 节点（如 node2）执行，target 在另一节点（如 node4）：

#### Prefill（segment_write，预填数据到 NoF SSD）

```bash
export SPDK_URMA_DEV_NAME=udmac0d1e2
export SPDK_URMA_CAPSULE_TRANSPORT=urma
export MC_HIFREQ_LOG_SAMPLE_RATE=0
export MC_SLICE_SIZE=1048576
export MC_STORE_MEMCPY=1 MC_STORE_MEMCPY_INLINE=1
export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib

./build/mooncake-store/benchmarks/stress_cluster_bench \
    --scenario=segment_write --protocol=tcp \
    --local_hostname=141.61.84.247:50370 \
    --metadata_server=http://141.61.84.151:8080/metadata \
    --master_server=141.61.84.151:50051 \
    --nof_segments='traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd trtype:URMA adrfam:IPv4 ns:1' \
    --num_keys=256 --value_size=4194304 --replica_num=0 --global_segment_size=0 --nof_replica_num=1 \
    --num_threads=16 --batch_size=16 --key_include_value_size=true \
    --nof_qpairs_per_segment=4 --nof_whole_pool_register=true --local_buffer_size=1073741824
```

通过：`All segments write complete: 256 succeeded, 0 failed` +
`Segment verification: errors=0`。
实测参考（node2→node4 单 SSD）：5.66 GiB/s，failed=0。

#### Read（segment_read，1-1 Get 基准）

```bash
# t1_b1（延迟基线）
./build/mooncake-store/benchmarks/stress_cluster_bench \
    --scenario=segment_read --role=reader --protocol=tcp \
    --local_hostname=141.61.84.247:50372 \
    --metadata_server=http://141.61.84.151:8080/metadata --master_server=141.61.84.151:50051 \
    --nof_segments='traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd trtype:URMA adrfam:IPv4 ns:1' \
    --num_keys=256 --value_size=4194304 --replica_num=0 --global_segment_size=0 --nof_replica_num=1 \
    --num_threads=1 --batch_size=1 --duration=20 --warmup_keys=5 --verify=false \
    --key_include_value_size=true --nof_qpairs_per_segment=4 --nof_whole_pool_register=true --local_buffer_size=1073741824

# t16_b16（吞吐基线）
./build/mooncake-store/benchmarks/stress_cluster_bench \
    --scenario=segment_read --role=reader --protocol=tcp \
    --local_hostname=141.61.84.247:50373 \
    --metadata_server=http://141.61.84.151:8080/metadata --master_server=141.61.84.151:50051 \
    --nof_segments='traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd trtype:URMA adrfam:IPv4 ns:1' \
    --num_keys=256 --value_size=4194304 --replica_num=0 --global_segment_size=0 --nof_replica_num=1 \
    --num_threads=16 --batch_size=16 --duration=20 --warmup_keys=5 --verify=false \
    --key_include_value_size=true --nof_qpairs_per_segment=4 --nof_whole_pool_register=true --local_buffer_size=1073741824
```

实测参考（node2→node4 单 SSD 4MB value）：
- t1_b1：4.03 GiB/s，1032 qps，failed=0，P50=950µs，P99=1.26ms
- t16_b16：13.90 GiB/s，3559 keys/s，failed=0，P50=72ms，P99=79.7ms

---

## 5. 1-1 Get Sweep 脚本（round11_930_branch）

### 5.1 脚本位置与文件

脚本在 **Windows 本机**：
```
D:\0_Projects\5_Mooncake\doc\3.Final_Performance_sorted\round11_930_branch\
├── run.py                        # 主执行器（适配 round10 11ssd_test/run.py）
├── remote.py                     # SSH 子进程/磁盘/RPC 助手（复用 round10 原文件）
├── analyze.py                    # 解析 bench 输出、写 summary.csv（复用 round10 原文件）
├── config_n2_to_n4.json          # 阶段1配置：t1_b1 × 5 reps × 60s
├── config_n2_to_n4_phase2.json   # 阶段2配置：t1_b16/t16_b1/t16_b16 × 5 reps × 60s
├── design.md                     # 实验设计 + 完整实测结果 + 判读
└── results_table.md              # 结果汇总表（用户格式）
```

### 5.2 脚本架构

`run.py` 从 Windows 本机通过 SSH 驱动远端节点：
1. `scp remote.py` 到每个节点 `/tmp/r11_remote.py`
2. 通过 `ssh ... python3 -u /tmp/r11_remote.py` 与远端交互（JSON 行协议）
3. 远端 `remote.py` 负责：磁盘枚举、vfio 绑定、SPDK RPC、进程启动/监控
4. `analyze.py` 解析 `stress_cluster_bench` 控制台输出，提取吞吐/延迟分位

### 5.3 config 参数说明

`config_n2_to_n4.json` 默认参数：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `device` | `udmac0d1e2` | URMA 设备名（c0 平面） |
| `reader` | `node2` | reader 节点（跑 stress_cluster_bench） |
| `source_order` | `["node4"]` | target 节点（跑 nvmf_tgt + SSD） |
| `scenarios` | `[1]` | 1-1 Get（单 source） |
| `modes` | `["B"]` | Mode B = NoF SSD 读写 |
| `combos` | `[[1,1]]` | (threads, batch) 组合；阶段2为 `[[1,16],[16,1],[16,16]]` |
| `reps` | `5` | 每个组合重复次数 |
| `duration` | `60` | 每次 run 持续秒数 |
| `smoke_duration` | `8` | smoke 模式持续时间 |
| `value_size` | `8388608` | 8 MiB |
| `strip_kib` | `256` | RAID0 条带大小 |
| `disks_per_target` | `11` | target 节点 SSD 数 |
| `target_cpus` | `2-7` | nvmf_tgt CPU 亲和（避开 CPU 0/1） |
| `spdk_root` | `/home/yin/nof/spdk` | SPDK 源码树（nvmf_tgt 路径） |
| `library_path` | `/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib` | 930 运行期 LD_LIBRARY_PATH（3 段） |
| `build_dir` | `/home/yin/nof/mooncake-930/build` | 930 build 目录 |
| `segment_size` | `17179869184` | 16 GiB（256 key × 8 MiB = 2 GiB 工作集，留余量） |
| `nof_inflight_bytes` | `50331648` | 48 MiB（segment 总预算） |
| `nof_workers` | `4` | NoF worker 线程数 |
| `nof_qpairs_per_segment` | `4` | 每 segment qpair lane 数 |
| `nof_submit_chunk_bytes` | `131072` | 128 KiB chunk |
| `memory_eid` / `target_eid` / `nof_reader_eid` | `0 / 0 / 0` | 全 EID0（单 source 不需要分流） |
| `ports` | `53000-54005` | RPC/metadata/admin/target/writer/reader 端口段 |

### 5.4 使用方法

#### 上传脚本到 reader 节点（node2）

```bash
# 从 Windows 本机上传到 node2
SSH="/c/Program Files (x86)/Git/bin/ssh.exe"
KEY=/c/Users/y00988740/.ssh/urma_diag_key

# 先创建目录
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 "mkdir -p /home/yin/round11_930"

# 上传 4 个文件
for f in run.py remote.py analyze.py config_n2_to_n4.json; do
  "$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
      "cat > /home/yin/round11_930/$f" \
      < "D:/0_Projects/5_Mooncake/doc/3.Final_Performance_sorted/round11_930_branch/$f"
done
```

#### 执行流程

```bash
# 在 node2 上执行（run.py 通过 SSH 驱动 node4 target）

cd /home/yin/round11_930

# 1. 只读盘点（确认 11 块 SSD 可枚举、端口空闲、设备 ACTIVE）
python3 run.py --discover --config config_n2_to_n4.json --out results/discover_n2_to_n4

# 2. smoke（8s、1 rep、开 verify，确认链路通+数据校验）
python3 run.py --execute --smoke --config config_n2_to_n4.json --out results/smoke_n2_to_n4

# 3. 阶段1 正式：t1_b1 × 5 reps × 60s
python3 run.py --execute --config config_n2_to_n4.json --out results/formal_n2_to_n4_t1b1

# 4. 阶段2 正式：t1_b16 / t16_b1 / t16_b16 × 5 reps × 60s（共 15 runs）
#    先上传 config_n2_to_n4_phase2.json
python3 run.py --execute --config config_n2_to_n4_phase2.json --out results/formal_n2_to_n4_phase2

# 5. 分析（run.py 结束时自动调用 analyze，也可手动）
python3 -c "from analyze import analyze; analyze('results/formal_n2_to_n4_t1b1')"
python3 -c "from analyze import analyze; analyze('results/formal_n2_to_n4_phase2')"
```

#### 脚本自动化行为

`run.py --execute` 会自动完成以下全部步骤，**无需手动部署**：

1. **盘点**：枚举两节点磁盘、端口、URMA 设备、二进制 SHA
2. **vfio 绑定**：将 target 节点 11 块 SSD 绑到 `vfio-pci`（记录原始驱动用于恢复）
3. **启动 nvmf_tgt**：`-m 0xfc`（CPU 2-7），`--wait-for-rpc`
4. **SPDK RPC 序列**：iobuf → framework_start_init → attach 11 块 NVMe →
   bdev_raid_create RAID0 strip=256 → nvmf_create_transport URMA →
   create_subsystem → add_ns → add_listener（精确 IP + IPv4）
5. **启动 master**：`--rpc_port=53000 --enable_http_metadata_server ...`
6. **NoF 注册**：`nof_register` 注册 NoF segment（16 GiB）
7. **Prefill**：`stress_cluster_bench --scenario=segment_write`（256 keys × 8 MiB）
8. **Measure**：每个 combo × rep 跑一次 `segment_read`，记录吞吐+延迟分位
9. **Teardown**：杀所有进程、恢复 vfio 绑定到 nvme 驱动
10. **Analyze**：写 `summary.csv` / `runs.csv` / `rejected.json`

⚠️ **端口冲突**：脚本默认用 53000-54005 端口段。如有其他进程占用会报
`port in use`，需先清理（kill 占用进程或改 config 的 ports 段）。

#### 产物结构

```
results/<phase>/
├── config.json                 # 实验配置快照
├── plan.json                   # 执行计划（所有 runs 列表）
├── reader_commands.json        # 每个 run 的完整 argv + env
├── inventory.json              # 两节点二进制 SHA/磁盘/设备/NUMA 拓扑
├── disk_candidates.json        # 自动选盘结果（11 块 BDF）
├── resolved_config.json        # 填入 BDF 后的配置
├── bindings.json               # vfio 绑定/恢复记录
├── r{0-4}_B/                   # 每个 rep 的目录
│   ├── node4_target.json       # SPDK target 状态（bdev/transport/reactors）
│   ├── node4_register.json     # NoF 注册结果
│   ├── node4_target.log        # nvmf_tgt 日志
│   ├── node4_writer.log        # prefill 日志
│   ├── node2_master.log        # master 日志
│   ├── r{N}_B_1-1_t{T}_b{B}.log  # bench 日志（含吞吐+延迟分位）
│   └── r{N}_B_1-1_t{T}_b{B}.json  # bench 结果元数据（argv/rc/result）
├── restore.json                # vfio 恢复结果
├── runs.csv                    # 所有 runs 的吞吐+延迟（CSV）
├── summary.csv                 # 按 combo 聚合的中位数/min/max
└── rejected.json               # 失败的 runs（空=全部有效）
```

#### 拉取结果到 Windows

```bash
SSH="/c/Program Files (x86)/Git/bin/ssh.exe"
KEY=/c/Users/y00988740/.ssh/urma_diag_key
WINBASE="D:/0_Projects/5_Mooncake/doc/3.Final_Performance_sorted/round11_930_branch"

for phase in formal_n2_to_n4_t1b1 formal_n2_to_n4_phase2; do
  mkdir -p "$WINBASE/results/$phase"
  for f in config.json plan.json summary.csv runs.csv rejected.json \
           disk_candidates.json restore.json bindings.json inventory.json \
           resolved_config.json reader_commands.json; do
    "$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
        "cat /home/yin/round11_930/results/$phase/$f 2>/dev/null" \
        > "$WINBASE/results/$phase/$f" 2>/dev/null
  done
done
```

### 5.5 实测结果（2026-11-03/04，930 分支，node2→node4，11 SSD）

| combo | 中位数 GiB/s | min-max % | P50 (µs) | P99 (µs) | P99.9 (µs) | valid |
|-------|-------------|-----------|----------|----------|------------|-------|
| t1_b1 | 23.37 | 6.2% | 279 | 754 | 860 | 5/5 |
| t1_b16 | 40.78 | 1.5% | 3,080 | 3,211 | 3,506 | 5/5 |
| t16_b1 | 46.85 | 0.002% | 2,458 | 5,636 | 6,291 | 5/5 |
| t16_b16 | 46.86 | 0.003% | 42,992 | 44,564 | 45,613 | 5/5 |

20 runs 全部 failed=0，restore 0 errors。t16_b1/t16_b16 撞 c0+EID0 链路容量墙
~47 GiB/s（skill 记录 46.6 GiB/s）。

---

## 6. 真实命令记录（本机 Windows Git bash → ssh.exe → 远程节点）

> 本节记录本次实验中**实际执行过的命令**，可作为复现的精确参考。
> 所有命令从 Windows Git bash 发起，通过 `ssh.exe` 到远程节点执行。
> 远程节点上的命令也可以直接在节点终端上跑（去掉外层 ssh 包装）。

### 6.1 本机 SSH 调用模板

```bash
# 本机唯一的 ssh.exe 路径（Git bash PATH 里没有 ssh）
SSH="/c/Program Files (x86)/Git/bin/ssh.exe"
KEY=/c/Users/y00988740/.ssh/urma_diag_key

# 标准调用（read-only 命令直接跑，写操作需授权）
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
    root@141.61.84.151 "<远程命令>"
```

⚠️ **引号规则**：外层双引号（本地 bash），内层单引号（远程 bash 正则/变量）。
本地 bash 会在双引号内展开 `$`，远程变量用单引号隔离或 `\$` 转义。
❌ 反例：`"...ssh.exe" ... "grep -nE "a|b" file"` —— 内层双引号被本地 bash 吃掉。

⚠️ **本机没有 scp.exe**：文件传输用 `ssh + cat` 管道（§6.7）。

### 6.2 编译阶段命令（node4 示例，node2 类似）

```bash
# === 克隆 930 分支 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof && \
     git clone --branch nof-gdr-supercache-snapshot-930 --recurse-submodules \
               https://github.com/LeeJace993/Mooncake.git mooncake-930 && \
     cd mooncake-930 && git log --oneline -1"

# === node2 克隆（需要代理覆盖）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "export http_proxy=http://127.0.0.1:3128 https_proxy=http://127.0.0.1:3128 && \
     export GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0=http.proxy GIT_CONFIG_VALUE_0=http://127.0.0.1:3128 \
            GIT_CONFIG_KEY_1=https.proxy GIT_CONFIG_VALUE_1=http://127.0.0.1:3128 && \
     cd /home/yin/nof && \
     git clone --branch nof-gdr-supercache-snapshot-930 --recurse-submodules \
               https://github.com/LeeJace993/Mooncake.git mooncake-930 && \
     cd mooncake-930 && git log --oneline -1"

# === venv + 依赖 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/mooncake-930 && \
     python3 -m venv ~/venvs/mooncake-bench && \
     source ~/venvs/mooncake-bench/bin/activate && \
     pip install -U pip && pip install aiohttp paramiko"

# === 修复源码 bug ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/mooncake-930 && \
     cp mooncake-store/src/client_service.cpp mooncake-store/src/client_service.cpp.bak_706551a8 && \
     sed -i '3046s/start_result.value().size()/start_result.value().replicas.size()/' \
         mooncake-store/src/client_service.cpp && \
     sed -n '3046p' mooncake-store/src/client_service.cpp"

# === cmake + make（node4）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/mooncake-930 && source ~/venvs/mooncake-bench/bin/activate && \
     mkdir -p build && cd build && \
     cmake .. -DUSE_NOF=ON -DUSE_NOF_URMA=ON -DUSE_UB=ON -DCMAKE_BUILD_TYPE=Release \
              -DUMDK_ROOT=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk \
              -DURMA_LIBRARY=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so && \
     make -j\$(nproc) && make install"

# === cmake + make（node2，URMA_ROOT 不同 + 代理）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "export http_proxy=http://127.0.0.1:3128 https_proxy=http://127.0.0.1:3128 && \
     export GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0=http.proxy GIT_CONFIG_VALUE_0=http://127.0.0.1:3128 \
            GIT_CONFIG_KEY_1=https.proxy GIT_CONFIG_VALUE_1=http://127.0.0.1:3128 && \
     cd /home/yin/nof/mooncake-930 && source ~/venvs/mooncake-bench/bin/activate && \
     mkdir -p build && cd build && \
     cmake .. -DUSE_NOF=ON -DUSE_NOF_URMA=ON -DUSE_UB=ON -DCMAKE_BUILD_TYPE=Release \
              -DURMA_ROOT=/usr/include/ub/umdk/urma \
              -DURMA_LIBRARY=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so && \
     make -j\$(nproc) && make install"
```

⚠️ **make 耗时**：`make -j$(nproc)` 在 308 核节点上约 3-5 分钟。如果通过 ssh 后台跑：
```bash
# 后台 make（避免 ssh 超时断开）
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/mooncake-930/build && \
     setsid nohup bash -c 'source ~/venvs/mooncake-bench/bin/activate && make -j\$(nproc) && make install' \
     > /tmp/r11_make.log 2>&1 < /dev/null & disown"
# 轮询完成：
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "tail -5 /tmp/r11_make.log; ls -la /usr/local/bin/mooncake_master"
```

### 6.3 部署阶段命令（node4 单节点 target+store）

```bash
# === vfio 绑定 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "modprobe vfio-pci && \
     echo vfio-pci > /sys/bus/pci/devices/0000:a4:00.0/driver_override && \
     echo 0000:a4:00.0 > /sys/bus/pci/drivers/nvme/unbind && \
     echo 0000:a4:00.0 > /sys/bus/pci/drivers_probe && \
     readlink /sys/bus/pci/devices/0000:a4:00.0/driver | xargs basename"

# === 启动 nvmf_tgt ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib && \
     export SPDK_URMA_DEV_NAME=udmac0d1e2 SPDK_URMA_CAPSULE_TRANSPORT=urma && \
     setsid nohup nvmf_tgt -m 0xfc --wait-for-rpc -r /var/tmp/spdk.sock \
         > /tmp/nvmf_tgt.log 2>&1 < /dev/null & disown"

# === SPDK RPC 序列 ===
# 注意：scripts/rpc.py 在 SPDK 源码树下
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/spdk && python3 scripts/rpc.py -s /var/tmp/spdk.sock rpc_get_methods"

"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/spdk && python3 scripts/rpc.py -s /var/tmp/spdk.sock iobuf_set_options \
        small_pool_count=12288 small_bufsize=8192 large_pool_count=512 large_bufsize=4194304 && \
     python3 scripts/rpc.py -s /var/tmp/spdk.sock framework_start_init"

"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/spdk && python3 scripts/rpc.py -s /var/tmp/spdk.sock \
        bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:a4:00.0"

"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/spdk && python3 scripts/rpc.py -s /var/tmp/spdk.sock \
        nvmf_create_transport -t URMA -c urma --max-io-size 4194304 \
        --iobuf-large-cache-size 32 --iobuf-small-cache-size 1024 \
        --dev-name udmac0d1e2 --eid-index 0"

"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "cd /home/yin/nof/spdk && \
     python3 scripts/rpc.py -s /var/tmp/spdk.sock nvmf_create_subsystem nqn.2026-01.io.spdk:mooncake-jd -a -s JD0001 && \
     python3 scripts/rpc.py -s /var/tmp/spdk.sock nvmf_subsystem_add_ns nqn.2026-01.io.spdk:mooncake-jd Nvme0n1 && \
     python3 scripts/rpc.py -s /var/tmp/spdk.sock nvmf_subsystem_add_listener nqn.2026-01.io.spdk:mooncake-jd \
         -t URMA -a 141.61.84.151 -s 4420 -f IPv4"

# === 启动 metadata + master + client ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "source ~/venvs/mooncake-bench/bin/activate && \
     export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib && \
     setsid nohup python -m mooncake.http_metadata_server --port 8080 > /tmp/metadata.log 2>&1 < /dev/null & disown && \
     sleep 1 && \
     setsid nohup mooncake_master --rpc_port=50051 --metrics_port=9003 \
         --enable_http_metadata_server --http_metadata_server_port=8080 \
         --rpc_address=141.61.84.151 --rpc_thread_num=32 \
         --eviction_high_watermark_ratio=0.9 --default_kv_lease_ttl=3600000 \
         --nof_heartbeat_failures_threshold=999999 --nof_heartbeat_interval_sec=3600 \
         > /tmp/master.log 2>&1 < /dev/null & disown && \
     sleep 2 && \
     setsid nohup mooncake_client --host 141.61.84.151 \
         --metadata_server http://141.61.84.151:8080/metadata \
         --master_server_address 141.61.84.151:50051 \
         --protocol ub --device_names udmac0d1e2 \
         --global_segment_size 4GB --threads 4 \
         > /tmp/client.log 2>&1 < /dev/null & disown"

# === NoF 注册 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib && \
     /home/yin/nof/mooncake-930/build/mooncake-store/src/nof_register \
         nqn.2026-01.io.spdk:mooncake-jd 1 141.61.84.151 4420 URMA 141.61.84.151:50051 10737418240"
```

### 6.4 冒烟测试命令

```bash
# === 通路级冒烟（在 target 节点 node4 上）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "export LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib && \
     export SPDK_URMA_DEV_NAME=udmac0d1e2 SPDK_URMA_CAPSULE_TRANSPORT=urma && \
     cd /home/yin/nof/mooncake-930 && \
     ./build/mooncake-store/tests/nof_buffer_alignment_test && \
     echo '---worker_pool_bench---' && \
     ./build/mooncake-store/benchmarks/nof_worker_pool_bench \
         --endpoints 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd ns:1' \
         --op=read --io_size=128K --iodepth=64 --duration_sec=20 --range_bytes=1G"

# === 跨节点 1-1 Get 冒烟（在 reader 节点 node2 上）===
# Prefill
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "export SPDK_URMA_DEV_NAME=udmac0d1e2 SPDK_URMA_CAPSULE_TRANSPORT=urma \
            MC_HIFREQ_LOG_SAMPLE_RATE=0 MC_SLICE_SIZE=1048576 \
            MC_STORE_MEMCPY=1 MC_STORE_MEMCPY_INLINE=1 \
            LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib && \
     cd /home/yin/nof/mooncake-930 && \
     ./build/mooncake-store/benchmarks/stress_cluster_bench \
         --scenario=segment_write --protocol=tcp \
         --local_hostname=141.61.84.247:50370 \
         --metadata_server=http://141.61.84.151:8080/metadata \
         --master_server=141.61.84.151:50051 \
         --nof_segments='traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd trtype:URMA adrfam:IPv4 ns:1' \
         --num_keys=256 --value_size=4194304 --replica_num=0 --global_segment_size=0 --nof_replica_num=1 \
         --num_threads=16 --batch_size=16 --key_include_value_size=true \
         --nof_qpairs_per_segment=4 --nof_whole_pool_register=true --local_buffer_size=1073741824"

# Read t1_b1
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "export SPDK_URMA_DEV_NAME=udmac0d1e2 SPDK_URMA_CAPSULE_TRANSPORT=urma \
            MC_HIFREQ_LOG_SAMPLE_RATE=0 MC_SLICE_SIZE=1048576 \
            MC_STORE_MEMCPY=1 MC_STORE_MEMCPY_INLINE=1 \
            LD_LIBRARY_PATH=/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib && \
     cd /home/yin/nof/mooncake-930 && \
     ./build/mooncake-store/benchmarks/stress_cluster_bench \
         --scenario=segment_read --role=reader --protocol=tcp \
         --local_hostname=141.61.84.247:50372 \
         --metadata_server=http://141.61.84.151:8080/metadata --master_server=141.61.84.151:50051 \
         --nof_segments='traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd trtype:URMA adrfam:IPv4 ns:1' \
         --num_keys=256 --value_size=4194304 --replica_num=0 --global_segment_size=0 --nof_replica_num=1 \
         --num_threads=1 --batch_size=1 --duration=20 --warmup_keys=5 --verify=false \
         --key_include_value_size=true --nof_qpairs_per_segment=4 --nof_whole_pool_register=true --local_buffer_size=1073741824"
```

### 6.5 验证与诊断命令

```bash
# === 检查进程存活 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "ps aux | grep -E 'nvmf_tgt|mooncake_master|mooncake_client|http_metadata' | grep -v grep"

# === 检查端口监听 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "ss -tlnp | grep -E ':50051|:8080|:9003|:4420'"

# === 检查磁盘驱动状态 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "for d in /sys/bus/pci/devices/*; do cls=\$(cat \$d/class 2>/dev/null); \
     if [ \"\$cls\" = \"0x010802\" ]; then bdf=\$(basename \$d); \
     drv=\$(readlink \$d/driver 2>/dev/null | xargs basename 2>/dev/null); \
     echo \"\$bdf \$drv\"; fi; done"

# === 检查 URMA 设备 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "urma_admin show --all 2>/dev/null | grep -E 'udmac|eid|ACTIVE' | head -20"

# === 检查 NoF segment capacity（master metrics）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "curl -s http://141.61.84.151:9003/ 2>/dev/null | python3 -m json.tool | head -30"

# === 检查 liburma 符号 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "nm -D /home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so | grep urma_register_seg_dmabuf && \
     echo '--- system lib (should be empty) ---' && \
     nm -D /usr/lib64/liburma.so 2>/dev/null | grep -c urma_register_seg_dmabuf"

# === 检查 cmake cache 关键 flag ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "grep -E 'USE_NOF|USE_NOF_URMA|USE_UB|CMAKE_BUILD_TYPE|URMA_ROOT|URMA_LIBRARY|UMDK_ROOT' \
     /home/yin/nof/mooncake-930/build/CMakeCache.txt"

# === 检查 nvmf_tgt 日志（listener 是否被拒）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "tail -20 /tmp/nvmf_tgt.log | grep -i 'does not allow\|error\|adrfam'"

# === 检查 mooncake_client 日志（RNIC 是否找到）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "grep -E 'HCA|RNIC|UbTransport|mock_urma|device_names' /tmp/client.log | head -10"
```

### 6.6 进程清理命令

⚠️ **清理前必须确认是自己的进程**，不要 kill 其他用户的进程（如 l00955908、z00926396）。

```bash
# === 查看自己的 mooncake 进程（排除其他用户）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "ps aux | grep -E 'mooncake|nvmf_tgt|http_metadata' | grep -v grep | \
     grep -v l00955908 | grep -v z00926396 | \
     grep -v '50056\|50058\|8086\|9006\|9008'"

# === Kill 自己的进程（先 SIGTERM，sleep 2，再 SIGKILL）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "kill <PID1> <PID2> <PID3> 2>/dev/null; sleep 2; kill -9 <PID1> <PID2> 2>/dev/null; \
     ps -p <PID1>,<PID2>,<PID3> -o pid 2>/dev/null; echo done"

# === 验证端口已释放 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "ss -tlnp | grep -E ':50051|:8080|:9003|:4420'"

# === 恢复 vfio 绑定到 nvme（如需手动恢复）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "echo nvme > /sys/bus/pci/devices/0000:a4:00.0/driver_override && \
     echo 0000:a4:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null; \
     echo 0000:a4:00.0 > /sys/bus/pci/drivers_probe && \
     readlink /sys/bus/pci/devices/0000:a4:00.0/driver | xargs basename"
```

### 6.7 文件传输命令（ssh + cat 管道）

本机没有 scp.exe，用 `ssh + cat` 替代：

```bash
SSH="/c/Program Files (x86)/Git/bin/ssh.exe"
KEY=/c/Users/y00988740/.ssh/urma_diag_key

# === 上传文件到远程节点 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "cat > /home/yin/round11_930/run.py" \
    < "D:/0_Projects/5_Mooncake/doc/3.Final_Performance_sorted/round11_930_branch/run.py"

# 批量上传
for f in run.py remote.py analyze.py config_n2_to_n4.json; do
  "$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
      "cat > /home/yin/round11_930/$f" \
      < "D:/0_Projects/5_Mooncake/doc/3.Final_Performance_sorted/round11_930_branch/$f"
  echo "uploaded $f"
done

# === 从远程拉取文件到本机 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "cat /home/yin/round11_930/results/formal_n2_to_n4_t1b1/summary.csv" \
    > "D:/0_Projects/5_Mooncake/doc/3.Final_Performance_sorted/round11_930_branch/results/formal_n2_to_n4_t1b1/summary.csv"

# 批量拉取
WINBASE="D:/0_Projects/5_Mooncake/doc/3.Final_Performance_sorted/round11_930_branch"
for phase in formal_n2_to_n4_t1b1 formal_n2_to_n4_phase2; do
  mkdir -p "$WINBASE/results/$phase"
  for f in config.json plan.json summary.csv runs.csv rejected.json \
           disk_candidates.json restore.json bindings.json inventory.json; do
    "$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
        "cat /home/yin/round11_930/results/$phase/$f 2>/dev/null" \
        > "$WINBASE/results/$phase/$f" 2>/dev/null
  done
done

# 拉取 bench 日志尾部（延迟分位摘要）
for rep in r0 r1 r2 r3 r4; do
  "$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
      "tail -40 /home/yin/round11_930/results/formal_n2_to_n4_t1b1/${rep}_B/${rep}_B_1-1_t1_b1.log 2>/dev/null" \
      > "$WINBASE/results/formal_n2_to_n4_t1b1/${rep}_B/${rep}_B_1-1_t1_b1_log_tail.txt" 2>/dev/null
done
```

### 6.8 Sweep 脚本执行命令

```bash
# === 在 node2 上执行 sweep 脚本 ===
# discover
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@141.61.84.247 \
    "cd /home/yin/round11_930 && python3 run.py --discover --config config_n2_to_n4.json \
        --out results/discover_n2_to_n4 2>&1"

# smoke（8s，1 rep，开 verify）
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
        -o ServerAliveInterval=10 -o ServerAliveCountMax=6 root@141.61.84.247 \
    "cd /home/yin/round11_930 && python3 run.py --execute --smoke \
        --config config_n2_to_n4.json --out results/smoke_n2_to_n4 2>&1"

# 阶段1 formal（t1_b1 × 5 reps × 60s，约 5-8 分钟）
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=8 root@141.61.84.247 \
    "cd /home/yin/round11_930 && python3 run.py --execute \
        --config config_n2_to_n4.json --out results/formal_n2_to_n4_t1b1 2>&1"

# 阶段2 formal（3 combos × 5 reps × 60s，共 15 runs，约 20-25 分钟）
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=12 root@141.61.84.247 \
    "cd /home/yin/round11_930 && python3 run.py --execute \
        --config config_n2_to_n4_phase2.json --out results/formal_n2_to_n4_phase2 2>&1"

# === 查看结果 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "cd /home/yin/round11_930 && cat results/formal_n2_to_n4_t1b1/summary.csv && \
     echo '=== runs.csv ===' && cat results/formal_n2_to_n4_t1b1/runs.csv"

# === 查看 log 尾部（延迟分位）===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "grep -E 'Total time:|Total queries:|Throughput \(avg\):|Queries/sec:|Min:|Avg:|P50:|P90:|P99:|P999:|P9999:|Max:' \
     /home/yin/round11_930/results/formal_n2_to_n4_phase2/r0_B/r0_B_1-1_t16_b1.log"
```

### 6.9 实验后清理与恢复验证

```bash
# === 检查 vfio 恢复结果 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.247 \
    "cat /home/yin/round11_930/results/formal_n2_to_n4_phase2/restore.json | \
     python3 -c 'import json,sys; r=json.load(sys.stdin); print(\"errors:\",[k for k,v in r.items() if v.get(\"errors\")])'"

# === 检查 node4 磁盘驱动已恢复 nvme ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "for d in /sys/bus/pci/devices/*; do cls=\$(cat \$d/class 2>/dev/null); \
     if [ \"\$cls\" = \"0x010802\" ]; then bdf=\$(basename \$d); \
     drv=\$(readlink \$d/driver 2>/dev/null | xargs basename 2>/dev/null); \
     echo \"\$bdf \$drv\"; fi; done"

# === 检查无残留 r11 进程 ===
"$SSH" -i "$KEY" -o StrictHostKeyChecking=no root@141.61.84.151 \
    "ps aux | grep -E 'r11|R11|nvmf_tgt.*0xfc|mooncake_master.*53000' | grep -v grep"
```

---

## 7. 陷阱速查

| # | 坑 | 处置 |
|---|---|---|
| 1 | `client_service.cpp:3046 .size()` 编译错误 | `sed -i '3046s/.size()/.replicas.size()/'`（§1.4） |
| 2 | 系统 `/usr/lib64/liburma.so` 缺 `urma_register_seg_dmabuf` | `-DURMA_LIBRARY=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk/lib/liburma.so` |
| 3 | node2 `UMDK_tool/include/` 为空 | 用 `-DURMA_ROOT=/usr/include/ub/umdk/urma` 指向系统头 |
| 4 | node2 git 坏代理 `141.1.139.187:8989` | `http_proxy=http://127.0.0.1:3128` + `GIT_CONFIG_COUNT` 覆盖（§1.2） |
| 5 | node2 cmake FetchContent 坏代理 | 同 #4，cmake 前设好代理环境变量 |
| 6 | nvmf listener adrfam=unknown | `nvmf_subsystem_add_listener` 必须加 `-f IPv4` |
| 7 | `nof_register` size=0 被拒 | 第 7 个参数 `size_bytes` 必须 > 0（如 `10737418240`） |
| 8 | `mooncake_client` `No available RNIC` | 加 `--device_names udmac0d1e2`（默认 `bonding_dev_0` 不可用） |
| 9 | `MC_HIFREQ_LOG_SAMPLE_RATE` 默认 1.0 全采样 | 所有进程显式 `=0` |
| 10 | `MC_NOF_INFLIGHT_BYTES_LIMIT` 是 segment 总预算 | 代码自动 ÷ lane_count；4qp 误设 8M → 2M/lane，吞吐掉到 ~21 GiB/s |
| 11 | SPDK 两端 commit 不一致（wire v3/v4 混用） | 两端都 `e12b7cb21`，`git log` 自检 |
| 12 | `mooncake_master --port` vs `--rpc_port` | 930 分支用 `--rpc_port`（`--port` 已 deprecated） |
| 13 | 端口冲突 `Duplicate rpc_meta key` | 每 bench 进程唯一端口；先 `ss -tlnp` 检查 |
| 14 | `LD_LIBRARY_PATH` 三段缺一 | `/home/yin/gdr/UMDK_netlab/lib:/usr/local/lib64:/usr/local/lib` |
| 15 | SPDK `make install` 报 `hatchling`/`uv` Error 2 | 不影响，以 `/usr/local/lib/libspdk_nvme.a` 存在为准 |
| 16 | SPDK `e12b7cb21+` `spdk_get_ticks` 链接回归 | `./configure` 加 `--disable-unit-tests` |

---

## 8. 一条龙 checklist

```
[ ] 两节点 SPDK e12b7cb21 安装并自检（nvme_urma.h + libspdk_nvme.a）
[ ] git clone 930 分支（node2 注意代理覆盖）→ HEAD 706551a8
[ ] venv 创建 + pip install aiohttp paramiko
[ ] 修复 client_service.cpp:3046（.size() → .replicas.size()）
[ ] cmake（node4: UMDK_ROOT / node2: URMA_ROOT + URMA_LIBRARY）→ make -j → make install
[ ] 验收：4 个二进制 + store.so + liburma 符号
[ ] vfio 绑定 SSD + nvmf_tgt -m 0xfc + RPC（-f IPv4 listener）
[ ] metadata + master（--rpc_port）+ client（--device_names udmac0d1e2）
[ ] nof_register（size_bytes > 0）
[ ] 冒烟①：nof_buffer_alignment_test 全 PASS
[ ] 冒烟②：nof_worker_pool_bench --op=read errors=0
[ ] 冒烟③：跨节点 prefill（segment_write）failed=0
[ ] 冒烟④：跨节点 read t1_b1 / t16_b16 failed=0
[ ] 正式：run.py --discover → --smoke → --execute（阶段1 t1_b1 + 阶段2 三 combos）
[ ] 拉取 results/ 到 Windows，对照 design.md 基线
```
