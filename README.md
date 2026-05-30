# XDPGate

XDPGate 是一个偏底层网络方向的 UDP 入口层实验项目。项目从一个最小的
ODE-based toy dedicated server 开始：客户端发送 input，服务端维护权威物理世界，
并通过 UDP snapshot 返回 cube 状态。后续逐步加入网络模拟、packet generator、
XDP gatekeeper、BPF maps 统计、`recvmmsg`、`SO_REUSEPORT` 和 benchmark 对比。

## 当前阶段

当前仓库处于设计和骨架准备阶段，尚未开始实现代码。

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
```

它会验证 INPUT、SNAPSHOT 和基础 header validation 的二进制编解码。
