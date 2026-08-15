# T113 Terminal 项目协作说明

## 项目概况

- 本项目面向全志 T113 平台，目标系统为 Linux。
- 本地开发环境运行在 WSL 中。
- 项目使用 `arm-linux-gnueabihf-gcc` 进行 ARM 交叉编译。
- CMake 工具链文件为 `toolchain-t113.cmake`。
- 程序入口为 `src/main.c`，构建目标名为 `t113_terminal`。

## 构建

在项目根目录执行：

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake
cmake --build build
```

生成的可执行文件为：

```text
build/t113_terminal
```

除非任务明确要求，不要删除或覆盖用户已有的构建产物和本地修改。

## T113 远程访问

- 通过 SSH 访问 T113 开发板。
- 当前已知 IP 为 `192.168.34.238`，但该地址可能变化。
- 不要把该 IP 硬编码到源码、脚本或构建配置中。
- 执行远程操作前，优先通过环境变量或 SSH 配置确认地址，例如：

```bash
export T113_HOST=192.168.34.238
ssh "$T113_HOST"
```

如果已在 `~/.ssh/config` 中配置主机别名，应优先使用别名。SSH 用户名、端口、密钥和远程部署目录以本地 SSH 配置或任务明确说明为准，不要猜测或提交敏感信息。

## 部署与验证

在确认目标地址和远程路径后，可按以下流程操作：

```bash
cmake --build build
scp build/t113_terminal "$T113_HOST":/tmp/t113_terminal
ssh "$T113_HOST" 'chmod +x /tmp/t113_terminal && /tmp/t113_terminal'
```

远程操作前应先确认网络连通性和目标设备身份。除非明确要求，不要执行会删除远程文件、重启设备或修改系统服务的操作。

## 修改约定

- 保持 CMake 配置与交叉编译工具链可用。
- 新增功能时优先修改 `src/`，不要把生成文件提交到源码目录。
- 修改后应至少检查 CMake 配置和本地可用的构建步骤；如果交叉编译器或目标板不可用，应明确说明未完成的验证项。
- 不要提交 SSH 私钥、密码、令牌或包含敏感信息的本地配置。
