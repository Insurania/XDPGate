# 第一阶段计划：ODE Toy Dedicated Server

第一阶段要搭出最小可用的 XDPGate DS 主流程：

```text
input packet -> 权威 ODE simulation -> snapshot packet -> viewer
```

这一阶段不做 XDP、AF_XDP、`SO_REUSEPORT`、`recvmmsg`、client prediction、
interpolation 或 reconciliation。

## 验收标准

第一阶段完成时应满足：

1. server 能启动 ODE world，包含 ground plane、一个 player cube 和若干 small
   dynamic cubes。
2. client 能通过 UDP 发送 `INPUT` packet。
3. server 能解码 input，并对 player cube 施加 force 或 impulse。
4. player cube 能和 small cubes 碰撞，并推动它们。
5. server 使用 fixed timestep 推进 ODE world。
6. server 周期性发送 `SNAPSHOT` packet。
7. viewer 能接收 snapshot，并渲染 cube 的位置和旋转。
8. protocol encode/decode 有基础单元测试。
9. README 说明 Ubuntu 22.04 上如何编译运行。

## 推荐技术栈

- C++17：用于 protocol、physics、server 和简单 client。
- CMake：用于构建。
- ODE：用于物理模拟。
- Python viewer：第一版快速迭代。

Ubuntu 22.04 后续可能需要：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libode-dev python3 python3-pip
```

这条命令会更新 apt 索引并安装 C++ 编译工具、CMake、ODE 开发库和 Python。人在
中国出差或使用云服务器时，`apt update` 可能因为镜像、网络或地区限制失败。
优先换 apt 镜像或稍后重试，不要一上来重装系统。

## 步骤 1：Protocol Library

创建：

```text
protocol/xdg_protocol.h
protocol/xdg_protocol.cpp
protocol/xdg_protocol_test.cpp
```

实现：

- header 常量。
- packet type enum。
- input payload 结构。
- snapshot payload 结构。
- input encode。
- input decode。
- snapshot encode。
- snapshot decode。
- 基础 validation error。

重要规则：

不要直接把 C++ struct `memcpy` 到网络包里，除非明确控制了 packing、alignment
和 endian。推荐写小工具函数，例如 `write_u32_le`、`read_u32_le`、`write_f32_le`
和 `read_f32_le`。

## 步骤 2：Physics World

创建：

```text
physics/entity.h
physics/ode_world.h
physics/ode_world.cpp
```

World 内容：

- Ground plane。
- Player cube。
- 8 到 16 个 small cube。
- Gravity。
- Collision space。
- Contact joint group。

建议模拟参数：

```text
tick rate: 60 Hz
fixed dt: 1 / 60
snapshot rate: 20 Hz
```

Input 行为：

- `move_x` 和 `move_z` 转成水平方向 force。
- `buttons bit 0` 可以触发短 impulse 或 boost。
- 将移动输入 clamp 到 `[-1, 1]`。
- input 不能直接覆盖 position 或 rotation。

Snapshot 提取：

- 对每个动态 cube，从 ODE 读取 position、quaternion rotation、linear velocity
  和 angular velocity。
- 转成协议层的 entity snapshot。

## 步骤 3：UDP Server

创建：

```text
server/main.cpp
server/ds_server.h
server/ds_server.cpp
server/udp_socket.h
server/udp_socket.cpp
server/input_queue.h
server/input_queue.cpp
```

Server loop：

```text
while running:
  接收当前可用的所有 UDP packet
  解码合法 INPUT packet
  记录 client endpoint

  while accumulated_time >= fixed_dt:
    消费最新 input
    将 input 应用到 physics world
    推进 physics world
    server_tick++

  if 到达 snapshot interval:
    encode snapshot
    send snapshot 到最新 client endpoint
```

第一阶段可以只支持一个 client endpoint。多 client 后续再做。

Socket 行为：

- 第一版使用普通 UDP socket。
- 使用 non-blocking receive。
- 第一版使用 `recvfrom`。
- `recvmmsg` 放到后续 benchmark 阶段。

## 步骤 4：Toy Client

创建：

```text
client/toy_client.cpp
```

职责：

- 以固定 input rate 发送 `INPUT` packet，例如 60 Hz。
- 递增 `input_sequence`。
- 如果方便，可以支持键盘输入。
- 第一版允许使用非交互 scripted mode。

初始 scripted mode 可以这样循环：

```text
向前移动 2 秒
向右移动 2 秒
向后移动 2 秒
向左移动 2 秒
重复
```

这样可以先验证 server 和 physics，不必一开始处理终端键盘输入。

## 步骤 5：Viewer

创建：

```text
viewer/snapshot_viewer.py
```

职责：

- 绑定或连接 UDP 端口接收 snapshot。
- 解码 `SNAPSHOT` packet。
- 渲染 cubes。
- 显示基础 stats，例如 server tick 和 entity count。

第一版 viewer 可使用简单 Python 渲染库。如果云服务器上 GUI 麻烦，viewer 应该
运行在 Windows 本地，server 运行在云服务器。

推荐开发分工：

```text
云服务器：运行 DS server
Windows 本地：运行 viewer，也可以运行 toy client
```

腾讯云轻量服务器带宽只有 5Mbps。远程测试时要控制 snapshot rate 和 entity 数量。

## 步骤 6：首次手动测试

本机单机测试：

```text
terminal 1: start server on 127.0.0.1:40000
terminal 2: start viewer
terminal 3: start toy client sending to 127.0.0.1:40000
```

预期结果：

- server log 显示收到 input packet。
- server tick 稳定增加。
- viewer 收到 snapshot。
- player cube 发生移动。
- small cubes 被物理碰撞推动。

云服务器测试：

```text
cloud: run server on 0.0.0.0:40000
local: run client/viewer against cloud public IP
```

云服务器测试前，需要在腾讯云防火墙/安全组里放行 UDP 端口。如果 packet 没到，
优先检查腾讯云安全组，然后检查 Linux 防火墙，再检查 client 目标 IP 和端口。

## 早期运行指标

server 每秒输出：

- 收到 packet 数。
- 有效 input 数。
- 非法 packet 数。
- 发送 snapshot 数。
- 当前 `server_tick`。
- simulation step 平均耗时。
- simulation step 最大耗时。
- last processed input sequence。

这些日志会让后续 benchmark 对比更容易。

## 风险

### ODE 安装和链接问题

不同发行版的 ODE 包名和链接参数可能略有差异。第一版 CMake 要保持简单，并在
README 里记录 Ubuntu 22.04 实测依赖。

### 云服务器防火墙混淆

UDP 可能被腾讯云安全组拦住，即使进程已经正确监听。先用本机 loopback 验证，
再只开放一个 UDP 端口做云端测试。

### 云服务器 GUI 问题

不要依赖在云服务器上跑图形界面。viewer 应该能在 Windows 本地运行，并接收
云服务器发来的 snapshot。

### 网络带宽限制

当前云服务器只有 5Mbps 带宽。远程测试要使用较低 snapshot rate 和较少 entity。

