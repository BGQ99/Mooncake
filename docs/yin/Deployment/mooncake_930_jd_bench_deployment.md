# Mooncake 930 分支编译部署 + JD Benchmark 测试流程

> 目标：把合并后的 `nof-gdr-supercache-snapshot-930` 分支（上游 930 + 我们的 NoF/URMA
> 实现 + JD bench 兼容层）编译部署到集群，然后用 JD 的
> **Mooncake-Performance** 基准套件（`python benchmark.py -f configs/basic/demo.json`
> 用法）跑冒烟与正式场景。
>
> - 代码：`~/nof/upstream/mooncake-supercache-nof-gdr-worktree`（分支
>   `nof-gdr-supercache-snapshot-930`，HEAD `d4ab36cb`，commit 链
>   abcc31fb → 83c710f5(env 兼容层) → dd41299f → 3e1b45f9(上游合并) → 089da5b8(bench 合并) → d4ab36cb(SPDK pin)）
> - bench：`~/nof/upstream/Mooncake-Performance`（编排机即可，勿需编译；语义见其 README）
> - 关联文档：[`../mooncake_nof_deployment.md`](../mooncake_nof_deployment.md)（部署踩坑总账）、
>   [`mooncake_deployment_gdr.md`](mooncake_deployment_gdr.md)（GDR 叠加）、
>   skill `mooncake-nof-bench`（集群拓扑 / 环境变量 / 历史基线）、
>   `Mooncake-Performance/README.md`（bench 全语义）
>
> 集群拓扑沿用 skill：node1 141.61.84.245 / node2 141.61.84.247 / node3 141.61.84.149 /
> node4 141.61.84.151，任意节点可任角色；SSH
> `ssh -i ~/.ssh/urma_diag_key -o BatchMode=yes -o StrictHostKeyChecking=no root@<ip>`。
> 下文以「**target 节点**（SPDK NoF target，挂 SSD）」「**store 节点**（master + metadata
> + store client）」「**client 节点**（仅 bench client）」指代，跑测试前先按用户指定定角色。

---

## 0. 进程总账（先读这节）

| 编号 | 进程 | 节点 | 启动方式 | 生命周期 | 存活确认 |
|---|---|---|---|---|---|
| P0 | `nvmf_tgt`（NoF target） | target | `sudo -E nvmf_tgt -m 0xfc` + RPC（§3.1） | 常驻；**target 重启后重来** | `ss -tlnp \| grep 4420` |
| P1 | `http_metadata_server` | store | `python -m mooncake.http_metadata_server --port 8080` | 常驻 | `curl http://127.0.0.1:8080/metadata` |
| P2 | `mooncake_master` | store | `./build/mooncake-store/mooncake_master --port 50051 --metrics_port 9003` | 常驻 | `ss -tlnp \| grep 50051` |
| P3 | NoF SSD 注册（一次性） | store | `python -m mooncake.mooncake_ssd_register --transport-type URMA …` | 跑一次即退 | master 日志/`:9003` 看到 SSD |
| P4 | `mooncake_client`（store 实例） | store | `./build/mooncake-store/mooncake_client --global_segment_size 4GB …` | 常驻 | 日志 SPDK env 初始化成功 |
| P5 | JD bench（编排机驱动） | client/store | `python benchmark.py -f …`（编排机发起，ssh 到各节点拉起 client 进程） | 按场景 | `outputs/<ts>/` 有 CSV |

⚠️ **master 重启后 NoF segment 丢失**（segment 挂在 master 内存里），P3 必须重跑；
target 重启则 P0 全套（vfio 绑定 + takeover + RPC + P3）重来。

---

## 1. 编译（store 节点；client 节点用 wheel）

只有 store 节点（要跑 master/store client/注册工具）需要完整编译；target 节点只装
SPDK；client 节点只装 mooncake Python 包（wheel，见 §1.4）。

### 1.1 依赖一键安装（store 节点 + target 节点）

```bash
cd ~/nof/upstream/mooncake-supercache-nof-gdr-worktree
sudo ./dependencies.sh -y --with-spdk --with-urma=<UMDK 安装前缀>
# 例（按各节点实际路径，find /home/yin/gdr -name urma_api.h 先定位）：
# sudo ./dependencies.sh -y --with-spdk --with-urma=/home/yin/gdr/UMDK_tool_netlab-sp4_umdk
```

脚本行为：系统包 → git submodule（含 **extern/yalantinglibs**，930 起在树内消费，
当前 pin `43948ffb`，必须包含我们的 `e46cbf08`）→ 从子模块构建安装 ylt → Go →
SPDK。SPDK 来源由分支内 `dependencies.sh` 钉死：

- repo `https://github.com/yyyuanhao426-hash/spdk.git`
- ref `e12b7cb213a9c1a2a22b99ab4732c8c11f95032c`（urma_enable_v2，qpair drain 收尾 +
  stage 遥测 + urma_stage_buffer，5 提交前移）

⚠️ **initiator/target 两端必须同一 commit**（wire v4 遥测 72B，v3/v4 不能混用）。
两端装完后都自检：

```bash
ls /usr/local/include/spdk/nvme_urma.h            # URMA 分支头文件
ls /usr/local/lib/libspdk_nvme.a
cd <spdk 源码树> && git log --oneline -1           # 必须 e12b7cb2…
```

⚠️ `make install` 末尾报 `hatchling`/`uv` Error 2 ≠ 失败（ADR-0018）：以
`/usr/local/lib/libspdk_nvme.a` 存在为准。

930 新增说明：`mooncake-common/common.cmake` 优先 `add_subdirectory(extern/yalantinglibs)`
在树内消费 ylt，不再依赖系统安装的 ylt 版本；老文档里「单独重建安装 yalantinglibs」
一步对编译不再是必需（dependencies.sh 里的安装对 wheel 打包仍无害）。
可选 `-DYLT_ENABLE_URMA=ON` 启用上游 URMA RPC transport（`MC_YLT_URMA_*`），
JD bench 走 tcp RPC + ub/NoF 数据面，**不需要**它。

### 1.2 编译 Mooncake（store 节点）

```bash
cd ~/nof/upstream/mooncake-supercache-nof-gdr-worktree
python3 -m venv ~/venvs/mooncake-bench && source ~/venvs/mooncake-bench/bin/activate
pip install -U pip && pip install aiohttp paramiko   # metadata server / 注册脚本运行期依赖

mkdir -p build && cd build
export UMDK_ROOT=/home/yin/gdr/UMDK_netlab   # cmake 期要头文件（含 is_gpu_seg 才有 GDR）；
                                             # 无 GPU Direct 需求也必须能找到 urma_api.h/liburma
cmake .. -DUSE_NOF=ON \
         -DUSE_NOF_URMA=ON \
         -DUSE_UB=ON \
         -DCMAKE_BUILD_TYPE=Release \
         -DUMDK_ROOT=$UMDK_ROOT
         # GPU Direct 叠加：再加 -DUSE_NOF_GPU_DIRECT=ON -DUSE_CUDA=ON
make -j$(nproc)
make install     # 二进制 + pybind store.so + mooncake_*.py 装进 configure 时的 venv
```

验收：

```bash
ls build/mooncake-store/mooncake_master build/mooncake-store/mooncake_client
python -c "from mooncake.store import MooncakeDistributedNoFRegister; print('store.so OK')"
# cmake 日志确认：USE_NOF_URMA=ON, SPDK URMA transport enabled for NoF
```

⚠️ **venv 一致性**：`make install` 装进 configure 时激活的 Python；后续运行 master、
注册脚本、bench 都用同一个 venv，否则 `from mooncake.store import ...` ImportError。

⚠️ **UMDK 两个路径可以不同**：cmake 期 `-DUMDK_ROOT` 要头文件；运行期
`LD_LIBRARY_PATH` 要 `liburma.so.0`（用 SPDK configure 时 `--with-urma=` 的同一前缀的
lib 目录）。`ldd` 确认不指向 `/usr/lib64` 旧版。

### 1.3 target 节点 SPDK

target 只需 §1.1 的 SPDK 步骤（可手动 clone 编译：`git clone -b urma_enable_v2
https://github.com/yyyuanhao426-hash/spdk.git` 后 checkout `e12b7cb21`，
`./scripts/pkgdep.sh && ./configure --with-rdma --with-urma=<UMDK> && make -j && make install`）。
**不需要** Mooncake 代码——注册脚本通过 SSH 读 target 的 listener RPC 信息。

### 1.4 mooncake Python 包分发（client 节点 + 编排机远端节点）

JD bench 的测试进程是 Python client（`store.so` 的 pybind 封装）。两种装法：

```bash
# 方式 A（推荐，多节点分发）：store 节点打 wheel 后拷贝安装
cd ~/nof/upstream/mooncake-supercache-nof-gdr-worktree
./scripts/build_wheel.sh            # 用法：./scripts/build_wheel.sh [python_version] [output_dir]
pip install dist/mooncake_transfer_engine-*.whl   # 在每个 client 节点执行

# 方式 B（单节点/与编译同机）：直接用 make install 装好的那个 venv
```

> wheel 构建需要完整 `build/`（engine.so + store.so 已编出），故先做 §1.2 的
> `make -j`。client 节点装 wheel 后同样需要：UMDK 运行库
> （`LD_LIBRARY_PATH`）、大页内存（USE_NOF 构建的 client 启动会初始化 SPDK env）。

---

## 2. 前置检查（所有参与节点）

```bash
# UB 栈内核模块与设备（两端）
lsmod | grep -E 'udma|ummu|ubcore|uburma'
ls /dev/uburma/udmac0d1e2                # 本环境 bonding_dev_0 不可用，用 udmac0d1e2

# 大页（两端都要；client 初始化 SPDK env 也吃大页）
sudo sysctl -w vm.nr_hugepages=2048 && grep Huge /proc/meminfo

# 编排机 → 所有节点 ssh 免密（bench 靠 ssh/scp 驱动）
ssh -i ~/.ssh/urma_diag_key root@<ip> true

# 端口预检：master 50051、metadata 8080、metrics 9003、bench client 9201..9200+N
ss -tlnp | grep -E '50051|8080|9003|920'
```

---

## 3. 部署启动

### 3.1 target 节点：NoF target（P0）

优先用 skill 的 `target_nvme_takeover.sh` 流程（BDF 动态解析、SIGTERM 60s+SIGKILL
fallback，见 `nof-bench.md` §4.7）；等价手动路径：

```bash
# target 节点；CPU 亲和避开 0/1（Round 8 结论，生产 pin 2-7 → mask 0xfc）
export SPDK_URMA_DEV_NAME=udmac0d1e2
export SPDK_URMA_CAPSULE_TRANSPORT=urma
sudo -E nvmf_tgt -m 0xfc &          # 或 setsid nohup 后台化，日志进 /tmp

sudo scripts/rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a <BDF>
sudo scripts/rpc.py nvmf_create_transport -t URMA
sudo scripts/rpc.py nvmf_create_subsystem nqn.2026-01.io.spdk:mooncake-jd -a -s JD0001
sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2026-01.io.spdk:mooncake-jd Nvme0n1
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2026-01.io.spdk:mooncake-jd \
     -t URMA -a <target 精确 IP> -s 4420     # ⚠️ 必须精确 IP，不能 0.0.0.0
```

也可在 store 节点用 `python -m mooncake.spdk_tgt_create --transport-type URMA …`
远程拉起（大 I/O 需 `--max-io-size/--iobuf-options`，见部署文档阶段 4）。

### 3.2 store 节点：master 三件套（P1/P2/P4）

每个进程独立窗口/pane 前台跑。公共 env：

```bash
export SPDK_URMA_DEV_NAME=udmac0d1e2
export SPDK_URMA_CAPSULE_TRANSPORT=urma
export LD_LIBRARY_PATH=<UMDK lib>:/usr/local/lib:$LD_LIBRARY_PATH
export MC_HIFREQ_LOG_SAMPLE_RATE=0        # 性能跑必须显式 0（默认 1.0 全采样刷屏）
export MC_SLICE_SIZE=1048576
export MC_STORE_MEMCPY=1 MC_STORE_MEMCPY_INLINE=1
```

```bash
# T2: P1 metadata
source ~/venvs/mooncake-bench/bin/activate
python -m mooncake.http_metadata_server --port 8080

# T3: P2 master
./build/mooncake-store/mooncake_master --port 50051 --metrics_port 9003

# T5: P4 store 实例（内存段载体；纯 NoF 时数据落 SSD，段给默认即可）
./build/mooncake-store/mooncake_client \
  --host <store 节点 IP> \
  --metadata_server http://<store 节点 IP>:8080/metadata \
  --master_server_address <store 节点 IP>:50051 \
  --protocol ub --global_segment_size 4GB --threads 4
```

### 3.3 NoF SSD 注册（P3，一次性，master 重启后重跑）

```bash
# store 节点，同一 venv；脚本 SSH 到 target 读 listener 信息
python -m mooncake.mooncake_ssd_register \
  --master_server_address <store 节点 IP>:50051 \
  --spdk_target_info "ip:<target IP> path:<target 上 spdk 源码目录>" \
  --transport-type URMA \
  --username root --key-file ~/.ssh/id_rsa
```

⚠️ `--transport-type` 默认 RDMA，**必须显式 URMA**，否则 listener 被过滤。
930 也新增了 C++ 直注工具：`nof_register <nqn> <nsid> <traddr> <trsvcid> <trtype>
<master_addr> [size_bytes]`（免 SSH 读 RPC，适合脚本化）。

验收：master `:9003` metrics / 日志出现注册的 SSD；`list_segments` 可见 NoF segment。

---

## 4. 冒烟测试（部署后 → JD bench 前）

部署完成后不要直接上 JD bench，先按**从底到顶**四级冒烟，每级通过再进下一级
（承接旧部署手册阶段 8 的验证阶梯；均为一次性运行，P3 注册完成后即可开始）。

### 4.1 通路级：SPDK endpoint 直测（store 节点）

```bash
# EP 与 §3.1 创建的 subsystem/nsid 对应；多 target 逗号分隔传给 --endpoints
export EP="trtype:URMA adrfam:IPv4 traddr:<target IP> trsvcid:4420 subnqn:nqn.2026-01.io.spdk:mooncake-jd ns:1"

# ① 对齐单测（512B/4K/128K/1M 对齐矩阵）
./build/mooncake-store/tests/nof_buffer_alignment_test

# ② host 内存 read 基线
./build/mooncake-store/benchmarks/nof_worker_pool_bench \
  --endpoints "$EP" --op=read --io_size=128K --iodepth=64 \
  --duration_sec=20 --range_bytes=1G
```

通过标准：①全部 PASS；②IOPS/带宽稳定输出、errors=0。GPU Direct 节点追加
③`--op=verify --memory=cuda`（fill→H2D→WRITE→READ→D2H→memcmp 的决定性测试，
先小 `--iodepth/--verify_rounds`，见旧部署手册 8.3）。

### 4.2 Store 级 KV 冒烟：显式 NoF 参数版（store 节点）

```bash
python mooncake-store/benchmarks/store_kv_bench.py \
  --scenario verify_write \
  --metadata-server http://<store 节点 IP>:8080/metadata \
  --master-server <store 节点 IP>:50051 \
  --local-hostname <store 节点 IP>:50071 \
  --protocol ub \
  --memory-replica-num 0 \
  --nof-replica-num 1
```

通过：read-after-write 校验 100%，无 staging fallback。
⚠️ `--memory-replica-num` 与 `--nof-replica-num` 不能同时 0（脚本参数校验直接报错）。

### 4.3 兼容层冒烟：env 覆盖路径（本分支核心，必做）

JD bench 不传任何 NoF 参数，全靠 `83c710f5` 的 env 覆盖——上 bench 前先用同一工具
证明覆盖路径真的生效（注意：**不带** `--memory-replica-num/--nof-replica-num`，
吃脚本默认值 mem=1/nof=0，看 env 能否覆盖它）：

```bash
MC_NOF_REPLICA_NUM=1 MC_MEM_REPLICA_NUM=0 \
python mooncake-store/benchmarks/store_kv_bench.py \
  --scenario verify_write \
  --metadata-server http://<store 节点 IP>:8080/metadata \
  --master-server <store 节点 IP>:50051 \
  --local-hostname <store 节点 IP>:50071 \
  --protocol ub
```

判读：

- 读写校验 100% 且数据**落 NoF**：master 日志/metrics 看副本 placement，client 侧
  `direct_ops`/`nof_ops` 计数（real_client summary）。若 100% 但 `nof_ops=0`
  （全走内存），说明 env 没到达进程——核对 export 的 shell/venv 与
  `/proc/<pid>/environ`。
- **阴性对照**：`MC_NOF_REPLICA_NUM=-1` 或 `=1abc` → 日志 ERROR（非法值）且
  **不覆盖**（回落脚本默认 mem=1），不回绕成超大数——收紧后的 parser 行为。
- **失败路径**：NoF segment 未注册时纯 NoF put **显式失败、不回落内存**（设计，
  见 §5.2）；若看到静默写进内存副本反而是 bug。

### 4.4 JD bench 冒烟

完成 §5 的 cluster.json / global.json / env.mooncake.sh 配置后，跑 demo 冒烟打通
端到端（命令见 §6 开头）：`python benchmark.py -f configs/basic/demo/simple_put.json
--protocol ub` → `outputs/<ts>/` 出 CSV。demo 仅 1×1 组合，客户端在 store1
（local 拓扑）；过了再上 simple_get / 正式目录。

---

## 5. JD bench 配置（编排机 `~/nof/upstream/Mooncake-Performance`）

### 5.1 工作副本与关键字段

```bash
cd ~/nof/upstream/Mooncake-Performance
cp configs/cluster.example.json configs/cluster.json    # 不入库
cp configs/global.example.json configs/global.json      # 不入库
```

**cluster.json**：`venvs`（节点上 venv python 路径，如
`{"mooncake-benchmark": "~/venvs/mooncake-bench/bin/python"}`）、`exclude_cpus`、
`nodes`。每个节点 `ip`（ssh 管理网，141.61.84.x）与 **`fabric_ip` 必填**（高性能网卡
IP，注入 `MC_FABRIC_IP`/`MC_TCP_BIND_ADDRESS`——缺省/选错会绑定到 Docker veth，
client 初始化挂死）。节点命名约定：`store1`（store 节点）、`node1`（非 store
client）、`writer_nodeX`/`reader_nodeX`（pipeline）。`ssh_key` 可填
`~/.ssh/urma_diag_key`。

**global.json**：

| 字段 | 本环境取值 |
|---|---|
| `master_type` | `"http"` |
| `master_addr` | `<store 节点 fabric IP>:50051` |
| `metadata_server` | `http://<store 节点 fabric IP>:8080/metadata` |
| `mooncake_store_nodes` | `["store1"]`（决定 local/remote 拓扑标注） |
| `venv` | `"mooncake-benchmark"`（cluster.json venvs 的键名） |
| `protocol.ub.device` | `"udmac0d1e2"`（example 里的 bonding_dev_0 本环境不可用） |
| `local_buffer_size` | ≥ num_threads × data_size（默认 1GiB；8MB×多线程时核对） |
| `numa_bind` | `true`（默认） |

### 5.2 NoF 副本注入（兼容层，本分支核心）

JD bench 的场景配置**没有** `nof_replica_num` 字段；本分支在
`Client::AttachHostId`（commit `83c710f5`）加了 env 覆盖点，所有写入口
（Put/Upsert/BatchPut/BatchUpsert）统一生效。在
`scripts/env.mooncake.sh`（git 跟踪的手工调优面板，随 env.sh 自动推送到所有节点、
runner 在 env.sh 之后 source）里加：

```bash
# --- NoF placement（83c710f5 兼容层）---
export MC_NOF_REPLICA_NUM=1     # 每个 object 的 NoF SSD 副本数
export MC_MEM_REPLICA_NUM=0     # 内存副本数；0+1 = 纯 NoF（SINGLE strict）
```

语义与纪律：

- **覆盖而非默认**：设了就覆盖调用方 config，JD bench 无感生效；不设 = 原行为。
- **纯 NoF 推荐 mem=0 + nof=1**：刻意不用 1+1 FLEXIBLE_DUAL_REPLICA（会静默退化到
  内存，掩盖问题）。NoF segment 未注册时 put **显式失败**，不回落——这是设计。
- 取值校验：仅非负十进制整数（`-1`/`1abc` 会报 ERROR 并拒绝，不会回绕）。
- ⚠️ vsegment 启用的部署对 nof≠0 直接 INVALID_PARAMS 不回落（rpc_service.cpp:542）。
- 读路径无需设置——副本 placement 在写时定档，get 跟随。
- 只测内存对照时把两行注释掉即可，无需重编。

### 5.3 NoF 传输旋钮（同一面板，按需）

```bash
export MC_NOF_WORKERS=4                    # ≥ qpairs
export MC_NOF_QPAIRS_PER_SEGMENT=4         # 4qp 突破到 ~43 GB/s 的关键
export MC_NOF_INFLIGHT_BYTES_LIMIT=25165824  # 4qp 用 24M（segment 总预算，自动÷lane）；1qp 用 32M
export MC_NOF_SUBMIT_CHUNK_BYTES=131072
export MC_NVME_IO_QUEUE_SIZE=256 MC_NVME_IO_QUEUE_REQUESTS=512
export MC_HIFREQ_LOG_SAMPLE_RATE=0         # 面板默认 0.01，性能跑改 0（面板在此文件改）
# 慢 I/O 归因（诊断时才开）：SPDK_URMA_STAGE_TRACE_US=10000 + MC_NOF_SLOW_TRACE_THRESHOLD_US=20000
# 两端 SPDK 必须 e12b7cb21（wire v4）才能配对
```

---

## 6. 运行

```bash
cd ~/nof/upstream/Mooncake-Performance

# 冒烟（JD demo，阶梯最后一关，见 §4.4；ub 协议 CLI 覆盖优先级最高）
python benchmark.py -f configs/basic/demo/simple_put.json --protocol ub
python benchmark.py -f configs/basic/demo/simple_get.json --protocol ub

# basic 正式（9 场景 × 22 并发组合 × 4 尺寸，较久；--no-plot 可跳过绘图）
python benchmark.py --config-dir configs/basic --protocol ub

# pipeline（精排/召排；demo 先行）
python benchmark.py --config-dir configs/pipeline/demo
python benchmark.py --config configs/pipeline/2-nodes/rank/rank_pulse_1w2r_1x_n2.json

# 其他常用：--dry-run 只打印命令；--no-warmup 关预热；--clean-up 清远程目录
```

阶段流：`put/put_batch/mixed` = generate-data → remove-keys → warmup → benchmark；
`get` 家族 = generate-data → prepare-store（预填）→ warmup → benchmark。
`put` 是 create-only，套件已保证先 remove-keys，不要手动绕过。

产物：`outputs/<ts>/<配置名>.csv`、`benchmark_detail.csv`、
`logs/<配置名>/*.log`（含 profiling/spdiag 树）、PNG 时延图。判读：

- `throughput_gb_s`（1024 进制）、`p99/p999/p9999_ms` 为核心列；
- 慢请求诊断：`python scripts/plot_latency.py --log outputs/<ts>/logs/<cfg>/<log>`；
- spdiag 打点树在 `logs/<cfg>/profiling/<节点>/spdiag_tree.<接口>.txt`；
- 与 skill 历史基线对照（NoF 4qp ~43 GB/s、host-to-host UB 最优 14.21 GiB/s 等，
  见 `mooncake-nof-bench` 各子文档 §9/§34）；记录实验按
  `experiment-report-template.md` 出 `docs/round<N>_….md`。

---

## 7. 陷阱速查（合并 skill + 两份部署文档的高频坑）

| # | 坑 | 处置 |
|---|---|---|
| 1 | SPDK 两端 commit 不一致（wire v3/v4 混用） | 两端都 `e12b7cb21`，`git log` 自检 |
| 2 | nvmf listener 绑 `0.0.0.0` | 必须精确 target IP，否则 sct=1 sc=132 |
| 3 | cluster.json 缺 `fabric_ip` | client 绑到 Docker veth，初始化挂死；必填业务网卡 IP |
| 4 | 注册时漏 `--transport-type URMA` | 默认 RDMA 会过滤掉 URMA listener，master 看不到 SSD |
| 5 | master / target 重启 | master 重启→重跑 P3 注册；target 重启→P0 全套重来 |
| 6 | `MC_HIFREQ_LOG_SAMPLE_RATE` 默认全采样 | master 和 bench 都显式 `=0`（static const，运行期改不了） |
| 7 | `MC_NOF_INFLIGHT_BYTES_LIMIT` 是 segment 总预算 | 代码自动 ÷ lane_count；4qp 误设 8M → 2M/lane，吞吐掉到 ~21 GiB/s |
| 8 | 端口冲突 `Duplicate rpc_meta key` | 每 bench 进程唯一端口；先 `ss -tlnp`，不 kill 现役端口 |
| 9 | 大页不足 | initiator 侧 client 也初始化 SPDK env，两端都 2048 |
| 10 | venv 不一致（make install 的 python ≠ 运行 python） | 全程同一 venv；wheel 路线注意目标节点 python 版本匹配 |
| 11 | NoF 要求连续 slice | MC_SLICE_SIZE=1MB；bench local_buffer 天然连续，勿拆散 |
| 12 | store 内存 ~12GB 限制 | 正式测量 key_count=1000；纯 NoF（mem=0）后数据落 SSD 无此压力 |
| 13 | vsegment 部署 + nof≠0 | 直接 INVALID_PARAMS 不回落，属预期 |
| 14 | `liburma.so.0` 指到 `/usr/lib64` 旧版 | 运行期 `LD_LIBRARY_PATH` 指向 SPDK configure 同款 UMDK lib |
| 15 | UB 设备名 | 本环境 `bonding_dev_0` 不可用（urma_create_device 失败），统一 `udmac0d1e2` |
| 16 | 多设备聚合 `udmac0d1e2,udmac1d1e2` | 触发跨卡读暴跌，禁止；单卡调参 `MC_WORKERS_PER_CTX/MC_MAX_WR` |
| 17 | store_kv_bench 两个 replica 参数同时 0 | 脚本参数校验直接报错（§4.2）；冒烟纯 NoF 必须 `--nof-replica-num 1` |

---

## 8. 一条龙 checklist

```
[ ] 两节点 SPDK e12b7cb21 安装并自检（nvme_urma.h + libspdk_nvme.a）
[ ] store 节点 cmake（USE_NOF/USE_NOF_URMA/USE_UB + UMDK_ROOT）+ make install + store.so OK
[ ] wheel 分发到 client 节点（或同 venv）
[ ] 两端大页 2048、UB 模块、/dev/uburma/udmac0d1e2
[ ] P0 nvmf_tgt + RPC（精确 IP listener）→ ss 4420
[ ] P1 metadata + P2 master + P4 store client 存活
[ ] P3 ssd_register --transport-type URMA 成功
[ ] 冒烟①：nof_buffer_alignment_test 全 PASS
[ ] 冒烟②：nof_worker_pool_bench --op=read errors=0（GDR 节点加 --op=verify --memory=cuda）
[ ] 冒烟③：store_kv_bench verify_write --memory-replica-num 0 --nof-replica-num 1 → 100%
[ ] 冒烟④：同命令去掉 replica 参数 + env 覆盖（MC_NOF_REPLICA_NUM=1/MC_MEM_REPLICA_NUM=0）→ 100% 且 nof_ops>0
[ ] cluster.json（fabric_ip!）/global.json（master_type/protocol.ub.device=udmac0d1e2）
[ ] env.mooncake.sh：MC_NOF_REPLICA_NUM=1 + MC_MEM_REPLICA_NUM=0 + NoF 旋钮
[ ] 冒烟⑤：benchmark.py -f configs/basic/demo/simple_put.json --protocol ub → CSV 出数
[ ] 正式：--config-dir configs/basic --protocol ub；结果对照 skill 基线并按模板归档
```
