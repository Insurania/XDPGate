# XDPGate 总体设计

XDPGate 是一个偏底层网络方向的 UDP 入口层实验平台。项目从一个最小的
ODE 权威物理专用服务器开始，先让真实游戏状态通过 UDP 协议流动起来，再
逐步加入网络模拟、包生成器、XDP 早期过滤、BPF maps 统计和 benchmark
对比能力。

第一阶段不追求极限 PPS。第一阶段的重点是让 packet payload 从一开始就
具有真实游戏语义：客户端发送 input，服务端维护权威 ODE 物理世界，snapshot
返回 cube 的位置、旋转、线速度和角速度。

## 目标

- 服务端运行权威 ODE physics world。
- 客户端通过 UDP input packet 控制 player cube。
- 玩家不能直接修改 cube 位置，只能通过 input 让服务端施加 force 或 impulse。
- player cube 可以和 small dynamic cubes 发生碰撞，并推动它们滚动、滑动或翻倒。
- 服务端周期性发送 binary snapshot。
- protocol、physics、server、viewer、benchmark、XDP/eBPF 分层清晰。
- 后续可对比无 XDP、开启 XDP、`recvmmsg`、`SO_REUSEPORT`、RSS 观测等模式下的
  DS 稳定性。

## 非目标

早期版本不要引入这些复杂能力：

- AF_XDP zero-copy。
- DPDK。
- SR-IOV。
- 完整游戏逻辑。
- 大地图。
- 动画系统。
- 复杂可靠 UDP 传输。
- 第一阶段客户端预测、插值和 reconciliation。
- 复杂负载均衡。
- 在小型云服务器上证明百万 PPS 极限性能。

## 目标环境

主要功能验证环境：

- 腾讯云轻量应用服务器。
- 2 核 4G。
- Ubuntu 22.04 LTS。
- 60G SSD。
- 5Mbps 带宽。

这个环境适合功能验证和小规模压测，不适合证明 XDP、AF_XDP、RSS、zero-copy
的极限性能。

因为这是云服务器，XDP native mode 可能受虚拟网卡和云厂商网络栈限制。XDP
阶段应优先支持 generic mode，确认链路和 counters 正确后，再检查是否支持
native mode。

## 总体架构

第一阶段链路：

```text
普通客户端 / toy client
        |
        | UDP input packets
        v
用户态 DS server
        |
        | ODE fixed timestep
        v
权威物理世界
        |
        | UDP snapshot packets
        v
viewer / client
```

后续 XDP 阶段链路：

```text
NIC / 虚拟网卡
        |
        v
XDP gatekeeper
        |
        | XDP_PASS
        v
Linux UDP 协议栈
        |
        v
用户态 DS server
```

XDP 只做早期过滤和统计，不解析完整 cube payload，也不做 ODE 或 game logic。

## 推荐项目结构

```text
XDPGate/
  AGENTS.md
  README.md
  docs/
    design.md
    protocol.md
    phase1-plan.md
    benchmark-plan.md

  protocol/
    xdg_protocol.h
    xdg_protocol.cpp
    xdg_protocol_test.cpp

  physics/
    entity.h
    ode_world.h
    ode_world.cpp

  server/
    main.cpp
    ds_server.h
    ds_server.cpp
    udp_socket.h
    udp_socket.cpp
    input_queue.h
    input_queue.cpp

  client/
    toy_client.cpp

  viewer/
    snapshot_viewer.py
    README.md

  tools/
    packet_gen.cpp
    net_sim.py
    stats_client.py

  xdp/
    xdp_gatekeeper.bpf.c
    xdp_loader.cpp
    maps.h

  bench/
    run_bench.py
    scenarios/

  CMakeLists.txt
```

## 模块边界

### `protocol`

负责 wire format、packet encode、packet decode 和基础校验。

该模块不依赖 ODE、socket、渲染、XDP 或 benchmark。

### `physics`

负责 ODE world、entity、rigid body、collision setup 和 fixed-step simulation。

该模块不直接读取 UDP packet。它只接收 server 层已经解码好的 input command。

### `server`

负责 UDP socket、server loop、input queue、client endpoint、simulation tick 和
snapshot 发送。

该模块调用 `protocol` 做 encode/decode，调用 `physics` 推进模拟。

### `client`

负责发送简单 input packet。第一阶段不需要 prediction、interpolation 或
reconciliation。

### `viewer`

负责接收 snapshot packet 并渲染 entity 状态。第一阶段只显示服务端 snapshot，
不做预测和插值。

### `tools`

放 packet generator、network simulator 和小型观测工具。工具应保持在 server
外部，避免污染 DS 主流程。

### `xdp`

放 BPF 程序和用户态 loader。XDP 只负责早期过滤和统计，不能包含 ODE/game logic。

### `bench`

放 benchmark 场景和脚本，用于对比 no-XDP、XDP、`recvfrom`、`recvmmsg` 和
`SO_REUSEPORT` 模式。

## 阶段规划

### 第一阶段：ODE Toy DS

- UDP input packet。
- 权威 ODE physics world。
- fixed timestep simulation。
- player-controlled cube。
- ground plane。
- 若干 small dynamic cubes。
- 周期性 snapshot packet。
- 最小 viewer。

### 第二阶段：网络模拟和包生成器

- 模拟 latency、jitter、packet loss、duplicate、reorder。
- 支持大量普通客户端。
- 生成合法协议包和非法 UDP flood 包。
- 观察不同网络条件下 snapshot、cube movement、碰撞和 DS tick 是否稳定。

### 第三阶段：XDP Gatekeeper

- 检查 UDP dst port。
- 检查 packet size。
- 检查 protocol magic。
- 检查 protocol version。
- 合法包 `XDP_PASS`。
- 非法包 `XDP_DROP`。
- 通过 BPF maps 记录 counters。

### 第四阶段：Benchmark 对比

- 对比 no-XDP 和 XDP 在非法 UDP flood 下的表现。
- 对比 `recvfrom` 和 `recvmmsg`。
- 加入 `SO_REUSEPORT` 多 worker 模式。
- 观察 CPU、应用层收到包数量、XDP drop 数量、DS tick 稳定性和 viewer 稳定性。

## 第一版实现建议

- 语言：C++17，用于 server、protocol、physics 和 packet tools。
- 物理引擎：ODE。
- 构建系统：CMake。
- viewer：优先 Python，方便快速迭代。
- XDP：后续使用 C 写 BPF 程序，用户态 loader 可用 C++/libbpf。
- 协议：固定小型 binary format，小端字段。

## 可观测性

DS 从第一阶段就应该每秒输出基础运行状态：

- 当前 `server_tick`。
- 每秒收到的 packet 数。
- 每秒处理的有效 input 数。
- 每秒发送的 snapshot 数。
- simulation step 平均耗时。
- simulation step 最大耗时。
- tick drift 或延迟 tick。
- 当前 client endpoint。

后续阶段再加入：

- XDP pass/drop counters。
- 按原因分类的 drop counters。
- benchmark 结果文件。
- snapshot latency 和 jitter 统计。

