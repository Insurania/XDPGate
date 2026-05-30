# XDPGate

XDPGate 是一个偏底层网络方向的 UDP 入口层实验项目。项目从一个最小的
ODE-based toy dedicated server 开始：客户端发送 input，服务端维护权威物理世界，
并通过 UDP snapshot 返回 cube 状态。后续逐步加入网络模拟、packet generator、
XDP gatekeeper、BPF maps 统计、`recvmmsg`、`SO_REUSEPORT` 和 benchmark 对比。

## 当前阶段

当前仓库已经具备最小 ODE DS、UDP input/snapshot、命令行 toy client 和 network
snapshot viewer。后续会继续加入 network simulator、packet generator、XDP 和 benchmark。

已完成文档：

- `docs/design.md`：总体设计。
- `docs/protocol.md`：UDP 协议 v1。
- `docs/phase1-plan.md`：第一阶段 ODE toy DS 计划。
- `AGENTS.md`：项目协作和中文注释规则。

## 目标环境

- Windows 本地：写代码、看文档、运行 viewer。
- 腾讯云轻量服务器：Ubuntu 22.04 LTS，用于编译运行 DS、网络实验和后续 XDP 验证。

## GitHub 仓库

仓库地址：

```text
https://github.com/Insurania/XDPGate.git
```

## 后续开发顺序

```text
protocol encode/decode
-> ODE world
-> UDP server
-> toy client
-> snapshot viewer
-> packet generator / network simulator
-> XDP gatekeeper
-> benchmark 对比
```

## 本地构建和测试

Windows PowerShell：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Ubuntu 22.04：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

当前测试目标：

```text
xdg_protocol_test
xdpg_physics_test
```

其中：

- `xdg_protocol_test` 验证 INPUT、SNAPSHOT 和基础 header validation 的二进制编解码。
- `xdpg_physics_test` 验证 ODE world 可以推进 tick、处理 input，并导出 entity state。

## 启动 DS Server

Windows 本地调试：

```powershell
cmake --build build --config Debug --target xdpg_server
.\build\server\Debug\xdpg_server.exe --port 40000 --small-cubes 15
```

Ubuntu 22.04 云服务器：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target xdpg_server
./build/server/xdpg_server --port 40000 --small-cubes 15
```

当前 UDP snapshot v1 使用 chunk 分包。单个 UDP packet 仍控制在 1200 字节以内，
但完整 snapshot 可以包含更多 entity。server 默认是：

```text
1 player cube + 180 small cubes
```

如果只想做低带宽远程测试，可以临时降低数量：

```powershell
.\build\server\Debug\xdpg_server.exe --port 40000 --small-cubes 15
```

当前 render snapshot 默认 60Hz，并使用压缩 entity wire format：
位置为 `uint16[3]` 量化，旋转为最小三项 `uint16[3]`，不发送线速度/角速度。
单个 entity 从早期调试格式的 60 字节降到 21 字节。

腾讯云轻量服务器只有 5Mbps 带宽。`180` 个 small cube 在 60Hz snapshot 下仍会产生
较多 UDP 包，适合功能验证，不适合长时间公网高频测试。

云服务器测试前，需要在腾讯云防火墙/安全组里放行对应 UDP 端口，例如
`40000/udp`。如果本地客户端连不上，优先检查云防火墙，再检查 Ubuntu 防火墙。

## 启动 Toy Client

`xdpg_toy_client` 是第一版命令行客户端，用于验证“客户端 input -> server ODE
模拟 -> snapshot 分包返回”的 UDP 主链路。它暂时不渲染画面，只打印收发统计。

Windows 本地连接本机 server：

```powershell
cmake --build build --config Debug --target xdpg_toy_client
.\build\client\Debug\xdpg_toy_client.exe --server 127.0.0.1 --port 40000
```

控制方式：

```text
W/A/S/D: 按住移动
Space: boost
Ctrl+C: 退出
```

无人值守验证可以使用 scripted 模式，客户端会自动发送移动输入并在指定时间后退出：

```powershell
.\build\client\Debug\xdpg_toy_client.exe --server 127.0.0.1 --port 40000 --duration-sec 3 --scripted
```

云服务器验证时，在服务器上运行 `xdpg_server`，本地用公网 IP 连接：

```powershell
.\build\client\Debug\xdpg_toy_client.exe --server <云服务器公网IP> --port 40000 --scripted
```

如果输出里 `completed_snapshots` 持续增长，说明 snapshot 分包已经能完整回到客户端。
如果只有 `sent_inputs` 增长而没有 `rx`，优先检查腾讯云安全组是否放行 UDP 端口。

当前 server 支持多个 toy client 订阅同一个权威世界的 snapshot，但还不是多人游戏：
所有客户端输入都会影响同一个 player cube。这个设计是为了先验证 UDP 入口、snapshot
分包和云服务器连通性；多 player entity、房间和输入归属会在后续阶段再拆出来。

## ODE 本地依赖

Windows 本地可以把 ODE 安装在项目目录下：

```powershell
New-Item -ItemType Directory -Force third_party\ode | Out-Null
git clone --depth 1 --branch 0.16.6 https://bitbucket.org/odedevs/ode.git third_party\ode\src
cmake -S third_party\ode\src -B third_party\ode\build -DCMAKE_INSTALL_PREFIX=third_party\ode\install -DBUILD_SHARED_LIBS=OFF -DODE_WITH_DEMOS=OFF -DODE_WITH_TESTS=OFF -DODE_WITH_LIBCCD=OFF
cmake --build third_party\ode\build --config Release --target INSTALL
cmake --build third_party\ode\build --config Debug --target INSTALL
```

Ubuntu 22.04 云服务器优先使用系统包：

```bash
sudo apt update
sudo apt install -y libode-dev
```

## Network Snapshot Viewer

当前 viewer 已改为网络 snapshot viewer：它不在本地运行 ODE simulation，只负责：

- 读取 W/A/S/D/Space 输入并通过 UDP 发送 INPUT packet；
- 接收 server 返回的 snapshot chunk；
- 等同一 `server_tick` 的分包收齐后渲染 cube 状态。

先启动 DS server：

```powershell
cmake --build build --config Debug --target xdpg_server
.\build\server\Debug\xdpg_server.exe --port 40000 --small-cubes 60
```

再另开一个 PowerShell 启动 viewer：

```powershell
cmake -S . -B build
cmake --build build --config Debug --target xdpg_ode_world_viewer
.\build\viewer\ode_world\Debug\xdpg_ode_world_viewer.exe --server 127.0.0.1 --port 40000 -notex
```

控制方式：

```text
W/A/S/D: 切换移动方向
Space: 切换 boost
Q 或 Esc: 退出
鼠标拖动: 调整相机
```

也可以开两个 viewer 窗口连接同一个 server：

```powershell
.\build\viewer\ode_world\Debug\xdpg_ode_world_viewer.exe --server 127.0.0.1 --port 40000 --client-id 1 -notex
.\build\viewer\ode_world\Debug\xdpg_ode_world_viewer.exe --server 127.0.0.1 --port 40000 --client-id 2 -notex
```

注意：当前还不是多人游戏。多个 viewer 会看到同一个权威 ODE world，并且输入都会作用到
同一个 player cube。后续再拆多 player entity 和输入归属。
