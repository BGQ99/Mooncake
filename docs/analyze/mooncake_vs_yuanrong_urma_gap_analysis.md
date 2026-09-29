# Mooncake vs yuanrong-datasystem：URMA 可靠性与 Master 均衡整改分析

> 对比范围：Mooncake `supercache-snapshot-912`（本轮 HEAD `82727559`）的 classic UB、TENT 与 `mooncake-store`；yuanrong-datasystem（本轮 HEAD `0c4c192c0`）的 URMA、对象缓存传输与集群控制。原文的细节证据基于两仓库较早的 `14a9f55a` / `4b19814c3`；本轮复核了关键路径，未逐项重测所有参数。源码位置随版本可能漂移。
>
> **证据口径**：下文“现状”指源码可见行为；“风险/收益”是推断，需故障注入和压测验证；UMDK/固件能力、参数阈值与线上 SLO 均待实机确认。文中用 `M:` / `Y:` 分别标注 Mooncake / yuanrong 仓库相对路径。本文只提出方案，不表示整改已实施。

## 一、结论与取舍

优先借鉴 yuanrong 的**故障事实确认、资源所有权和调度反馈闭环**，而不是移植其单设备数据面或分布式 Master。Mooncake 已具备多 HCA、集中式围栏选举、OpLog/快照、动态副本、SSD 分层和人工 Drain/Move 框架；当前最急的是 classic UB 的故障恢复与超时资源收敛，其次是利用现有任务框架做可恢复的自动再平衡。

| 议题 | yuanrong 可借鉴机制 | Mooncake 现状/边界 | 建议 |
|---|---|---|---|
| 端口与对端故障 | 端口快照/epoch、待验证态、peer 熔断、incarnation 围栏 | classic UB 事件置 inactive 后每秒强制激活；多 HCA 可继续服务 | 先修复活振荡，再分离设备、peer 和资源背压三类状态 |
| 超时与资源 | 保留迟到完成、孤儿 WR 记账、jetty 退役 | 超时后 batch 仍 busy，避免立即 UAF，但 CQE 丢失时无界等待；staging 部分失败分支不收敛 | 先修确定性泄漏，再建完成记录/隔离账本 |
| 降级 | UB→TCP、预算限制、整请求决策 | Store 协议按 segment 静态选择；写超时结果可能模糊 | READ 优先换副本；WRITE 仅在可证明未 post 或事务可裁决时改道 |
| Master 负载 | 压力/热度反馈、迁移限速、拓扑围栏 | 有人工 Drain、Move/Copy、动态副本；无自动 planner | 保留集中式 Master，在任务框架上加 shadow planner 和可恢复意图 |
| 数据面性能 | per-peer 配额、批聚合、gather、NUMA 反馈 | Mooncake 有多 HCA；TENT 已有策略与指标 | 正确性稳定后分项压测，性能增强优先评估 TENT |

**两类“均衡”必须分开**：其一是 Master CPU/QPS/扫描压力，其二是 segment 容量、热度与副本分布。yuanrong 的分布式元数据哈希环针对前者也承担成员扩缩容；Mooncake 不应为解决后者而仓促替换自己的集中式 HA。Master 本身的瓶颈应先量化，再做索引、批处理与读扩展。

### 1.1 数据面归属与改造范围

| 路径 | 已有能力 | 本轮职责 |
|---|---|
| classic UB | Store `protocol=ub` 的直接路径；多 HCA context、staging、endpoint 缓存 | 承担现网正确性修复；健康、完成和关闭语义不能只在新数据面实现 |
| generic RDMA | 原子深度记账与 rail 失败处理 | 复用类型化错误和故障域隔离思想；不重复修 classic 专属缺陷 |
| TENT | 选择器、QoS、NIC 负载统计与 UB 原生数据面测试 | 长期统一控制面候选；新增性能能力优先与其比较 |

`MC_USE_TENT` 是显式启用路径（`M:mooncake-transfer-engine/src/transfer_engine.cpp:401-417`）。因此文中提到“无闭环选路”“`volatile` 深度”等问题时，默认只指 classic UB；反之，TENT 有统计不代表 Store 现网 classic UB 已享有该能力。推荐把错误分类、健康快照和 completion 语义定义在传输中立接口，由各实现映射，而不是令 Store 直接依赖某个 provider 的状态码。

## 二、可靠性：机制、差距与设计边界

### 2.1 端口健康不是“收到错误后睡一秒”

yuanrong 把 provider 状态查询封装在 `UrmaPortStatusProvider`，通过不可变健康快照和 `healthEpoch` 发布已确认变化；`verificationPending` 不用于解除既有隔离。CQE 4/9 分别被解释为本地端口和远端确认线索，再通过本地刷新或远端 `QueryUbPortHealth` 验证；peer incarnation 防止重启前的坏状态污染新连接（`Y:src/datasystem/common/rdma/urma_port_status_provider.cpp`、`Y:src/datasystem/common/object_cache/ub_port_health.cpp`、`Y:src/datasystem/common/object_cache/peer_ub_admission.cpp`、`Y:src/datasystem/cluster/ub_health/remote_ub_port_health_verifier.cpp`）。**CQE 数字与 BONDP opcode 是实现相关 ABI，不能进入 Mooncake 公共状态机。**

它的控制链并非一个“定时查状态”函数，而是：异步事件/CQE 形成线索 → provider 查询和远端验证形成事实 → 发布带 epoch 的健康摘要 → 准入与路由只消费新事实 → 半开探针验证恢复。若远端验证超时，保持 pending 和既有隔离，不用一次普通成功覆盖此前已确认的坏端口。这个“线索与事实分离”比具体轮询周期更值得迁入。

Mooncake classic UB 的 `UbWorkerPool::monitorWorker()` 每秒无条件 `set_active(true)`（`M:mooncake-transfer-engine/src/transport/kunpeng_transport/ub_context.cpp:617-625`），可能形成“失败—复活—再失败”循环。建议以 `(device, port, health_epoch)` 为本地健康事实，状态为 `UNKNOWN/PENDING/HEALTHY/UNHEALTHY`；仅被确认的全坏 device 停止新 post，部分坏口仍可由 bonding 或其余 HCA 承载。事件与轮询失败不可被旧快照覆盖。provider 不支持查询时保留事件模式并明确报 `UNKNOWN`，不能假报健康。

实施前在构建节点对 `urma_api.h`、`urma_ubagg.h`、`liburma` 与固件做 compile/link/runtime capability probe。头文件能找到不等于 `BONDP_USER_CTL_QUERY_PORT_STATUS` 可用。先以 shadow 模式记录“若启用将隔离谁”，故障注入证明不会误隔离后再 enforce。

### 2.2 对端熔断与安全降级

yuanrong 区分本地端口坏、远端不可达、池耗尽，并以 per-peer 配额、退避和 HALF_OPEN 单探针限制坏对端的重试半径（`Y:src/datasystem/common/rdma/urma_resource.h`、`src/datasystem/common/object_cache/peer_ub_admission.cpp`）。Mooncake classic UB 的 slice 多次失败后删除 endpoint，但下一个请求仍可能立即重连；TENT 已有策略、QoS 和 NIC 统计，不能把 classic 的 gap 归到整个 Mooncake（`M:mooncake-transfer-engine/src/transfer_engine.cpp:803-818`）。

建议由传输适配层产生类型化结果：`LOCAL_DEVICE_UNHEALTHY`、`PEER_UNREACHABLE`、`RESOURCE_BACKPRESSURE`、`AMBIGUOUS_COMPLETION`、`REMOTE_VERSION_STALE`。breaker 键包含 peer incarnation，且仅对可归因于该 peer 的失败计数；资源池耗尽只触发退避，不使所有 peer 熔断。Store 消费语义结果，不直接理解 CQE 数字。READ 可优先换同版本可读副本，再评估有预算的 TCP；已 post 的 one-sided WRITE 超时可能部分完成，**不得由传输层盲目 TCP 重放**，须由对象版本/lease/事务 token 裁决。

建议 breaker 状态为 `CLOSED → OPEN → HALF_OPEN → CLOSED/OPEN`。进入 OPEN 只接受明确可归因的远端故障证据；OPEN 期间快速拒绝该 peer 的新建连；冷却后仅一笔探针进入 HALF_OPEN；peer 重启、健康 epoch 前进或路由对象版本变化都应先检查代际。退避及冷却要可配置并暴露指标，但不能靠“连续成功 N 次”绕过已确认全坏端口。

降级预算至少限制进程级在途字节与单请求规模，并记录拒绝原因；具体上限由基线测定。元数据 RPC 也用 TCP，降级不能挤占其线程和带宽。先支持“可证明未 post”的 WRITE 和可校验的 READ，模糊 WRITE 暂返回明确错误。

### 2.3 业务超时不等于 DMA 结束

yuanrong 对超时后的事件、孤儿 WR、迟到 CQE 和 lane 退役分别记账；`PostGate` 使准入与退役在同一原子线性化点完成（`Y:src/datasystem/common/rdma/urma_resource.h`、`urma_resource.cpp`）。Mooncake `MultiTransport` 对外报告 TIMEOUT 时不立即释放仍在途的 batch，这避免了简单的“超时即释放”UAF；真正缺口是 CQE 永久丢失时资源无上界，以及 staged slice 在直接 `markFailed()` 分支可能绕过最终结算（`M:mooncake-transfer-engine/src/multi_transport.cpp:113-126,274-327`、`kunpeng_transport/ub_context.cpp:242-280,530-539`）。

建议先统一 staged slice 失败出口和关停顺序，再把 WR 的 `user_ctx` 从可复用 slice 指针演进为稳定的 `CompletionRecord(request, generation, resource_refs, settled_once)`。业务回调可以先结束，record、staging 和 remote-write 目标缓冲需等 CQE/flush/销毁确认后释放；超时但在途的 WR 进入按**条目数与字节数双限额**的隔离账本。上限触发时停止准入并退役相关 jetty/context，而非删账本后复用内存。固定等待 128ms 等 yuanrong 常量不是安全证明。classic UB 的 `volatile` 深度表也应原子化；generic RDMA 已是原子类型，不重复改造（`M:mooncake-transfer-engine/include/transport/kunpeng_transport/urma/urma_endpoint.h`、`M:mooncake-transfer-engine/include/transport/rdma_transport/rdma_endpoint.h`）。

生命周期门槛：**完成身份/一次性结算 → 有界隔离 → PostGate/安全退役 → lane 池化**。若颠倒顺序，池化会放大 ABA 与迟到 DMA 风险。每 HCA 独立 context 应采用 per-context 池加 per-peer 配额，而非照搬 yuanrong 的进程级 200 lane 池。

可把 WR 生命周期定义为 `CREATED → POSTING → POSTED → CQE/FLUSH`；同步 post 返回部分接受时，已接受 WR 按 POSTED 对账，未接受 WR 立即完成本地结算。`TIMEOUT` 只改变业务可见状态，不能让 POSTED 资源回池。record 的一次性结算位同时保护正常 CQE、迟到 CQE、取消与关停竞争；若 flush 和销毁也无法确认释放，则隔离至进程退出并发出高优先级告警。这里的“有界”指正常服务可用容量和新准入有界，并非一定能安全回收所有不确定硬件资源。

### 2.4 故障模型对比

Mooncake 的围栏选举、顺序 OpLog、分块快照保障集中式 Master 的状态复制与恢复；yuanrong 的 lease、拓扑版本、见证探测和 self-fencing 更多针对 worker 嵌入式分布式 Master 的网络分区。两者并非简单“谁更可靠”。Mooncake 可在误判 worker 死亡的成本显著时引入跨故障域 witness，但先验证现有 leader epoch、重连与副本可读围栏。**不要直接复制 `SIGKILL` 自杀或哈希环成员语义。**

| 故障 | Mooncake 优先核对 | 可借鉴 yuanrong 的点 |
|---|---|---|
| 本地 device 或端口故障 | 多 HCA 中仅故障域退出；任务转其它 rail | 健康 epoch、部分坏口继续服务、恢复验证 |
| 对端重启/分区 | 旧 endpoint、旧副本描述和旧 completion 不复用 | incarnation、远端健康验证、peer 探针 |
| Master 切主 | OpLog/快照恢复后不重复调度；副本首读可见性保持 | 拓扑版本拒旧、任务意图幂等、拓扑批活动时暂停再平衡 |
| 元数据后端自身分区 | leader lease/围栏有效性与客户端重试语义 | 必要时跨域见证，但不能替代一致性协议 |

## 三、Master 均衡：具体差距与可行方案

### 3.1 当前基础与真正瓶颈

Mooncake 已有 `CreateCopyTask`、`CreateMoveTask`、动态副本和 `CreateDrainJob`；Drain 会避开 hard-pin、未过期 lease、未完成副本与已有复制任务（`M:mooncake-store/src/master_service.cpp:13871-14203`）。缺的是自动候选生成、反馈限速、切主后的调度意图恢复。`drain_jobs_` 当前是进程内 map（`M:mooncake-store/include/master_service.h:3007`），因此不应把“定时调用 Drain”当成可靠的自动均衡。

Master CPU 侧还有一个具体成本：`ScheduleDrainJobTasks()` 遍历 shard→tenant→key→源 segment 来统计候选/阻塞项；即使计划槽位填满，仍继续扫描以形成阻塞集合（`M:mooncake-store/src/master_service.cpp:14075-14185`）。集群对象数增长后，该路径可能占用 Master CPU/锁；这是源码推断，需用扫描耗时、键数与锁等待实测。读路径已有 `BatchGetReplicaList()`（同文件 `:4612`），因此不能笼统声称“只支持单 key RPC”。

yuanrong 的均衡哈希环、拓扑版本重定向、内存/热度再平衡与迁移速率限制证明“容量报告→候选→迁移→反馈”可形成闭环（`Y:src/datasystem/cluster/algorithm/balanced_ring.h`、`Y:src/datasystem/master/memory_rebalance_scheduler.cpp`、`Y:src/datasystem/master/heat_rebalance_scheduler.cpp`）。但 `enable_memory_rebalance` 在源码中默认关闭（`Y:src/datasystem/common/flags/common_flag_define.cpp:233`）；不能把代码存在当成默认生产能力，更不能把 80%/20%/40MB/s 等示例阈值直接复制。

yuanrong 的内存均衡不是看到源节点超过阈值就立即搬迁：它区分源/目标余量、热度、迁移速率与拓扑变更；批间刷新容量报告，避免计划使用过期余量。热度均衡还需避免把热点从一台机器转移到另一台。Mooncake 复用其**反馈和限速**，而不复用“由哈希环决定元数据 owner”的调度前提。

### 3.2 三种方案对比

| 方案 | 做法 | 收益/代价 | 判断 |
|---|---|---|---|
| A：保留集中式 Master，增强扫描与任务调度 | 索引/增量候选、批处理、可恢复 planner、限速迁移 | 复用现有 HA/副本模型；改动边界清楚，但仍须测单 leader 上限 | **近期首选** |
| B：在 A 上增加只读扩展 | follower 或缓存承接可容忍滞后的读；用 leader epoch/version 保证新鲜度 | 可降读 QPS；一致性、失效和 lease 语义复杂 | **只在读压实测为瓶颈时做** |
| C：移植分布式 Master/哈希环 | 元数据所有权按 worker 分散 | 潜在水平扩展；需重做 OpLog、快照、围栏、跨分片事务和副本调度 | **当前不建议** |

推荐先做 A：采集 Master CPU、RPC QPS/p99、锁等待、全表扫描耗时、元数据条目数、任务队列水位；若 Drain/planner 扫描占比高，先引入按 segment/状态的候选索引或增量队列，避免每轮全表扫描。只有在读 QPS 是主要瓶颈且批量 RPC/缓存不足时，才评估 B。C 属架构级重写，不能作为本轮整改前提。

具体落地分两条工作流，避免“降低 Master CPU”与“均衡存量数据”互相掩盖：

1. **Master CPU/QPS**：按 RPC 类型记录排队、处理、锁等待和序列化时间；Drain/planner 记录扫描键数、候选数和每轮耗时。先减少全量扫描频率，把状态变化写入按 segment 分组的候选索引或增量队列。索引只作为加速层，最终下发 MoveTask 前仍回主元数据校验，避免索引陈旧导致错误迁移。
2. **存储压力/热度**：每轮消费带时间戳的 segment 容量与热度报告，计算源/目标收益并输出过滤原因；所有对象状态在提交任务前重新读取。迁移不会降低 Master 峰值 QPS，反而可能增加写放大，所以 planner 必须有 CPU、任务数、字节数三重预算。

如采用 B，只允许**可容忍短暂陈旧且不授予新 lease** 的读先试点；写、pin、leader 决策仍回 leader。缓存/只读副本要携带 leader epoch 和对象 version，异常时 fail-closed 回源；否则“读扩展”会破坏恢复副本首读与 lease 围栏。

### 3.3 自动再平衡的最小安全闭环

推荐流程：容量/热度报告 → shadow 候选 → 过滤与预算 → 带 leader epoch 的可恢复意图 → 现有 Move/Copy Task → 目标首次可读且 version 匹配 → 按源释放保护 → 重新读取余量并进入下一批。

建议把一轮 planner 控制在小的、可中止的预算内：先对**源 segment** 按利用率/热度/故障域打分，再在该源的候选对象中按“预期释放字节 ÷ 搬迁成本”排序；目标优先选不同故障域且保有可验证余量的 segment。计划不跨多个报告代际无限延长，提交前重查对象、目标容量和 leader epoch；容量不足、拓扑变更或任务队列积压时暂停新任务。起始阈值只用于 shadow 比较，正式执行前由实际分布与业务 p99 标定。

最小任务状态应覆盖 `PLANNED → RESERVED → COPYING → READABLE → SOURCE_RELEASED` 及 `CANCELLED/FAILED`。预留容量与保护源副本需同意图绑定；目标仅“传输完成”不等于 READABLE，必须通过元数据发布/版本校验。重试沿用稳定意图 ID，不重新挑选一个可能产生双份保护的 source。任务终态后释放预算并记录被迁移字节、等待和失败原因。

候选必须检查 tenant quota、hard/soft pin、对象 lease、client liveness/offboarding、动态副本 lease/version、已有 task、目标故障域、内存/SSD tier、可读副本下限和目标预留容量。拓扑变化或报告过期时暂停新迁移，已在途任务继续按幂等规则结算。迁移字节/并发、每源/目标冷却、SSD 下刷和动态副本共享预算；**优先迁移低热度且收益明确的对象**，避免 planner 本身制造热迁移风暴。

supercache-snapshot 的两个安全语义必须保留：

1. 每个 SSD 下刷队列对应的**源副本独立保护**，不能用一个任务的保护位代表多个源；只有该 source 对应目标首次可读、版本匹配且任务状态已结算，才释放该 source。已被任何镜像 worker 领取时不得整体取消排队任务（`M:mooncake-store/include/master_service.h:2227-2239`）。
2. 恢复副本首次可读时继续通过 `RefreshRecoveredSoftPin()` 重新授予 snapshot soft-pin TTL；自动 planner 不得绕开该索引/指标与 TTL 语义（`M:mooncake-store/include/master_service.h:1494`、`M:mooncake-store/src/master_service.cpp:1409`）。

HA 意图只选一个真相源：优先把 planner intent/状态写入现有 OpLog/快照，并以 `(tenant,key,source,target,object_version,leader_epoch)` 去重；若选择“切主后从 DRAINING segment 和任务反推”，必须证明覆盖部分执行、取消、超时和重复调度。切主前后任何时刻都不能留下永久 DRAINING、丢失逐源保护或重复迁移同一版本。

这里的 leader epoch 用于**拒绝旧 leader 的新调度**，不是让新 leader 丢弃旧 epoch 已完成的有效数据；恢复时先重放意图和 task 状态，再按新 epoch 继续或安全撤销。需要单独演练“迁移已完成但可读状态未发布”“目标已可读但源保护未释放”“下刷镜像已领取而取消到达”三种切主窗口，验证幂等性。

## 四、性能借鉴：可靠性达标后再做

| 方向 | yuanrong 做法 | Mooncake 最小实验与决定条件 |
|---|---|---|
| 反馈选路 | chip inflight 差和内存亲和 | 在多 HCA 上按 device 的 inflight bytes/完成带宽做 shadow 评分；先过滤坏设备，防一个 peer 拥塞污染整 HCA；与 TENT 已有指标复用 |
| 池化/公平 | lane 池、per-peer 上限、退役余量 | completion ledger 稳定后试 per-context 小池；分别限制 peer、总 lane、孤儿字节；用 p99 与故障半径评估 |
| 小对象批量 | gather write、BatchGet 聚合与分配失败二分 | 先在 TENT 或独立实验路径做 A/B；`max_sge`、post 部分接受和对象级失败必须逐项核对 |
| 内存/设备路径 | 预注册池、流水线 MSet、H2D pipeline | Mooncake 先解决 staging 终态和 shutdown 次序，再测 CPU staging 与 H2D 的真实瓶颈 |

### 4.1 哪些实现细节值得借鉴

**内存池与 NUMA**：yuanrong 客户端传输缓冲采用预注册 arena，页对齐并按 NUMA/chip 分配；传输时用地址范围表选源 chip，避免每次系统调用。Mooncake 已有按 NUMA 注册的 segment 和多设备拓扑发现，因此不需要另建一套整进程注册池；若小对象注册/拷贝开销确实占比高，再针对客户端临时缓冲做专用池实验（`Y:src/datasystem/common/rdma/urma_manager.cpp`，`M:mooncake-transfer-engine/src/topology.cpp`）。

**批量失败不应放大**：yuanrong BatchGet 将对象打包写入聚合缓冲，分配失败时下降分配上限并二分子区间，最终可对叶子批做 TCP 处理；MSet 用有界窗口先提交再等待（`Y:src/datasystem/client/object_cache/transport/data_plane/ub_transporter.cpp`）。Mooncake 可借鉴“保持批内对象级结果、失败只缩小受影响区间”，但要核对 batch_get 的副本版本、TTL 和调用方 partial-success 语义，不能仅合并 WR 数量。

**gather 与完成取证**：yuanrong 利用多个 SGE 和 WR 链减少 post 次数，同时按 `badWr` 区分实际接受数；分相 trace 覆盖 post、poll、通知和观察延迟（`Y:src/datasystem/common/rdma/urma_manager.cpp`）。Mooncake 已有 SpDiag/trace_id；先补 CQE 与 worker 唤醒阶段的时间戳，确认瓶颈确在 post 或调度，再决定是否做 gather。若 provider 上限或已注册内存布局不匹配，保留单 SGE 路径。

**GPU/H2D**：yuanrong 的流水化路径复用 URMA context/JFC 并把网络完成与设备拷贝解耦；Mooncake 当前 CPU staging/H2D 仍须先解决终态和关停安全。流水化只有在设备侧 copy 明显占 p99、且 completion ledger 可安全持有两个阶段资源时才值得实现。

yuanrong 的 128ms 延迟回池、200 lane、每 peer 8 lane、inflight 差 15、40MB/s 迁移速率均是其硬件/负载下的参数样本，不是 Mooncake 默认值。`bthread`、单 `bonding_dev_0`、分布式 Master 的实现也不宜照搬。Mooncake classic UB 的测试并非零：`ub_transport_test` 存在但未注册 ctest，TENT 另有原生 UB 用例；差距是可重复的故障注入矩阵（`M:mooncake-transfer-engine/tests/CMakeLists.txt:310-317`）。

性能取舍还需注意“准零拷贝”与安全生命周期的冲突。yuanrong 的 Get 可以写入预注册池并由外部 owner 管理缓冲；Mooncake 若缩短 staging 路径，必须先证明对象发布、DMA 结束、缓冲释放三者顺序正确。Gather 和 batch 聚合也需保留子对象独立的错误与版本结果；若单个大批失败就整体重放，会抵消性能收益并增加不确定写风险。

## 五、整改计划与验收门槛

以下按**依赖关系**排序，每阶段拆成小 PR；不把性能/自动调度与传输正确性改动捆绑。阶段 0 的实测结果决定阈值及是否推进后续阶段。

| 阶段 | 可交付改动 | 前置/灰度 | 完成标准 |
|---|---|---|---|
| 0：基线与注入 | classic UB 用例注册 ctest；mock 能模拟端口 flap、post 部分接受、CQE 乱序/丢失、flush/delete 失败、关停在途；采集 Master 和数据面基线 | 本地实现，远端构建/测试；UMDK 能力探测 | 每个后续故障有可复现失败用例；吞吐、CPU、锁等待、超时/泄漏水位有基线 |
| 1：确定性正确性 | 统一 staged slice 终态、修 shutdown/inflight 顺序、classic 深度原子化；引入类型化错误及完成代际 | 不改变路由；先测后改 | 失败/取消/超时后计数恰好结算一次；ASan/TSan 与在途关停测试通过 |
| 2：故障闭环 | 健康快照与 provider 适配、per-peer breaker、完成/隔离账本、READ/安全 WRITE 降级预算 | 健康 shadow→enforce；降级单独开关；完成身份先于池化 | 全坏 device 阻止新 post、其它 HCA 可用；peer 故障不扩散；丢失 CQE 不导致无界占用或提前复用 |
| 3：Master 自动均衡 | 扫描成本优化、shadow planner、可恢复意图、限速/cooldown、逐源保护 | 先只记录候选和过滤原因；leader 切换演练后小比例执行 | pin/lease/version/SSD 并发约束保持；切主不重复/丢任务；容量倾斜下降且业务 p99 不恶化超预算 |
| 4：性能优化 | 多 HCA 反馈选路、per-context 池、gather/聚合、H2D 流水 | 仅选有瓶颈证据的项；优先比较 TENT | A/B 证明吞吐或 p99 收益，CPU/内存/故障半径未恶化 |

### 5.1 可拆分的实施包

| 包 | 主要代码入口 | 首个验证点 | 合入边界 |
|---|---|---|---|
| R1：staging 终态 | `kunpeng_transport/ub_context.cpp`、`ub_transport.cpp` | initial dispatch / redispatch 失败后 `completed_slices` 收敛；关停时无在途 UAF | 不改变协议选路 |
| R2：完成身份与资源账本 | `include/transport/kunpeng_transport/urma/urma_endpoint.h`、`multi_transport.cpp` | post 部分接受、迟到/丢失 CQE 的 generation 对账 | 不依赖池化；先限制账本字节/条目 |
| R3：健康与熔断 | `kunpeng_transport/ub_context.*`、公共错误接口 | PORT_DOWN 不自动复活；peer 重启后旧 breaker 无效 | 先 shadow，独立开关 enforce |
| R4：语义安全降级 | Store 的读副本选择、传输路由 | READ 换副本和限流；模糊 WRITE 不重放 | 不修改数据发布事务语义，除非另有 RFC |
| M1：Master 成本画像 | `master_service.cpp` 的 Drain/读 RPC | 扫描键数、锁等待、RPC p99 的归因 | 只加指标；不改副本选择 |
| M2：shadow planner | Master segment/对象元数据与现有 task API | 计划/过滤原因可重现；不下发真实任务 | 索引陈旧时以主元数据重新校验 |
| M3：可恢复执行 | OpLog/快照、Move/Copy Task、逐源保护 | 切主、取消、部分完成、SSD 下刷并发 | 小比例启用，具备暂停/回滚开关 |

R1/R2 是 R3/R4 的安全基础，M1/M2 可与传输工作并行，M3 需先确定 HA 意图真相源。R1 修复范围必须覆盖所有直接失败出口，不能只补当前最容易触发的一条分支。R2 的新 record 应能携带 provider 原始诊断用于日志，但公共错误只暴露稳定语义。R3 的 shadow 数据要同时展示事件判定与 provider 判定不一致的样本，避免只看成功率。

### 5.2 Master 阶段的具体试运行顺序

1. **画像期**：按 segment 输出容量分位、任务积压、读写热度、SSD 下刷带宽；按 Master RPC 类型输出 CPU 与 p99。确认失衡确实存在，且不是个别对象/租户突发造成的短时现象。
2. **影子期**：planner 每轮只记录候选 `(source,target,key-hash,version,bytes)` 与过滤原因，不写 task；对同一份报告重复运行，应给出相同决策。将计划收益和搬迁成本与人工 Drain 结果比较。
3. **单任务期**：限制每轮、每源/目标和全局并发，先只处理无 hard/soft pin、无进行中复制、单 tier 的简单对象；任一拓扑版本变化立即停止发新任务。
4. **扩展期**：再纳入 soft-pin、SSD 与动态副本组合；逐源保护、恢复 soft-pin TTL、目标首次可读判断各有独立测试；切主恢复和取消逻辑通过后才扩大比例。

回滚是**停止生成新计划**，不等于强删已在途任务：已提交的迁移仍按原意图对账，必要时由新 leader 安全撤销。若指标显示 Master 扫描耗时、任务积压或业务 p99 越线，应暂停 planner 并保留现场意图和隔离资源，以便追查，而不是靠重启清状态。

### 5.3 验收口径

正确性是硬门槛：数据校验、可读副本下限、无双释放/UAF、无旧 epoch 写入、无永久 DRAINING，以及自动调度关闭后不改变既有行为。性能是比较门槛：以相同对象数、请求分布、HCA/SSD 配置做无故障与故障注入 A/B，记录 p50/p99、CPU、Master 锁等待、网络与 SSD 带宽。改善容量倾斜却显著放大 Master p99 不能算成功。

对可恢复性，应覆盖每个任务状态的切主点并核验“重放后意图数、活跃任务数、逐源保护数、预算占用数”一致。对于无法在 mock 中证明的 DMA 停止、provider port-status ABI 与固件故障恢复，必须在 `work@mooncake-dev` 实机验证；未通过时保持 shadow 或关闭相关 enforce 开关。

**核心故障验收矩阵**：

| 注入场景 | 不变量与观察点 |
|---|---|
| 单端口 flap、本地全端口坏、peer 重启 | pending/旧 epoch 不解除隔离；全坏只影响所属 device；旧 incarnation 状态不能用于新 peer |
| post 部分接受、CQE 乱序/迟到/永久丢失 | 每 WR 只结算一次；业务按时返回；隔离条目/字节有界；旧 generation 不能写入复用缓冲 |
| shutdown-inflight、flush/delete 失败 | 未确认 DMA 停止前不注销内存；不能安全释放则隔离并可观测；无 UAF/双释放 |
| WRITE 超时与 TCP fallback 风暴 | 模糊完成不盲重放；进程/peer/租户预算生效，元数据 RPC 不被饿死 |
| rebalance 与 pin/lease/SSD 下刷/切主并发 | 被过滤对象不迁移；逐源保护和恢复 soft-pin TTL 保留；意图可幂等恢复 |

建议统一观测键 `trace_id + request/generation + peer_incarnation + device + tenant/key hash + object_version`；至少输出健康 epoch/pending、breaker 转换、orphan/quarantine 字节与年龄、fallback 拒绝、Master 扫描耗时/锁等待、候选过滤原因、迁移字节与切主重建数。无故障性能回退预算与恢复 SLO 由阶段 0 数据协商，本文不拍固定数值。

## 六、实施前待裁决

1. **硬件与 ABI**：远端 UMDK 头/库/固件是否同代并支持端口查询；不支持时健康逻辑如何以事件模式降级。
2. **数据面归属**：classic UB 继续作为现网路径必须回补 P0；lane 池、选路、批量性能增强选择 TENT 还是 classic，需以迁移窗口和 A/B 决定。
3. **写语义**：Store 现有 version/lease/token 能否裁决 one-sided WRITE 模糊完成；不能则不开放该类自动 TCP 重放。
4. **Master 意图**：OpLog/快照持久化与状态反推二选一作为唯一真相源；明确 leader epoch 与逐源保护的恢复语义。
5. **上线阈值**：breaker 退避、孤儿/隔离容量、迁移预算与 SLO 均需实机故障注入和生产基线确认。

*本轮只修订分析与计划，未修改实现；具体源码行号以对应 checkout 为准。*
