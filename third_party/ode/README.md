# 项目本地 ODE 依赖

本目录用于放置项目本地 ODE 依赖，避免强制修改系统环境变量。

当前推荐版本：

```text
ODE 0.16.6
```

Windows PowerShell 安装方式：

```powershell
New-Item -ItemType Directory -Force third_party\ode | Out-Null
git clone --depth 1 --branch 0.16.6 https://bitbucket.org/odedevs/ode.git third_party\ode\src
cmake -S third_party\ode\src -B third_party\ode\build -DCMAKE_INSTALL_PREFIX=third_party\ode\install -DBUILD_SHARED_LIBS=OFF -DODE_WITH_DEMOS=OFF -DODE_WITH_TESTS=OFF -DODE_WITH_LIBCCD=OFF
cmake --build third_party\ode\build --config Release --target INSTALL
cmake --build third_party\ode\build --config Debug --target INSTALL
```

在 Windows 上同时安装 Release 和 Debug 是为了避免 MSVC 静态库 runtime 不匹配。

Ubuntu 22.04 上可以优先使用系统包：

```bash
sudo apt update
sudo apt install -y libode-dev
```

如果希望 Ubuntu 也使用项目本地 ODE，可以使用同样的源码构建方式，只是最后安装命令为：

```bash
cmake --build third_party/ode/build --config Release --target install
```

以下目录是本地生成内容，不提交到 Git：

```text
third_party/ode/src/
third_party/ode/build/
third_party/ode/install/
```
