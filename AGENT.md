# T113 Terminal 项目协作说明

## 项目概况

- 本项目面向全志 T113 平台，目标系统为 Linux。
- 本地开发环境运行在 WSL 中。
- 项目使用 `arm-linux-gnueabihf-gcc` 进行 ARM 交叉编译。
- CMake 工具链文件为 `toolchain-t113.cmake`。
- 程序入口为 `src/main.c`，构建目标名为 `t113_terminal`。

## 构建

首次构建或 `build/` 目录为空时，先配置交叉编译：

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-t113.cmake
```

VS Code 的构建任务只执行增量构建，不会自动创建或重新配置 CMake 构建目录。

也可以直接执行：

```bash
cmake --build build -j
```

对应 VS Code 任务为 `T113: Build`，使用 GCC 问题匹配器显示编译错误。

生成的可执行文件为：

```text
build/t113_terminal
```

除非任务明确要求，不要删除或覆盖用户已有的构建产物和本地修改。

## VS Code 远程调试

项目的 `.vscode/launch.json` 和 `.vscode/tasks.json` 配置了 `T113 Remote Debug` 调试流程：

```text
T113: Build
    -> T113: Deploy
        -> T113: Start gdbserver
            -> VS Code 连接 gdbserver
```

当前配置的具体信息：

- 本地调试程序：`${workspaceFolder}/build/t113_terminal`
- 本地工作目录：`${workspaceFolder}`
- 调试器：`/usr/bin/gdb-multiarch`
- 调试方式：GDB/MI (`MIMode: gdb`)
- 远程 GDB 服务：`192.168.34.238:2345`
- 调试启动时在程序入口暂停（`stopAtEntry: true`）
- 需要在 WSL 中安装 `gdb-multiarch`，并在 VS Code 中安装 C/C++ 扩展

点击 `T113 Remote Debug` 前，必须确保：

- WSL 可以使用 `cmake`、`arm-linux-gnueabihf-gcc`、`scp`、`ssh` 和 `gdb-multiarch`；
- T113 板端已安装并可执行 `gdbserver`；
- WSL 可以通过 SSH 以 `root` 用户访问目标板，且已配置好认证；
- 目标板的 `2345` 端口可从 WSL 访问。

## T113 远程访问

- 通过 SSH 访问 T113 开发板。
- 当前已知 IP 为 `192.168.34.238`，但该地址可能变化。
- 当前 VS Code 配置使用 `root@192.168.34.238`，不要把这个地址当作永久地址。
- 如果 IP 发生变化，需要同步修改 `.vscode/launch.json` 中的 `miDebuggerServerAddress`，以及 `.vscode/tasks.json` 中 `T113: Deploy` 和 `T113: Start gdbserver` 的 SSH/SCP 地址。
- 不要把 SSH 密钥、密码或令牌提交到仓库。

手动检查连接时，可以使用环境变量保存当前地址：

```bash
export T113_HOST=192.168.34.238
ssh root@"$T113_HOST"
```

如果已在 `~/.ssh/config` 中配置主机别名，应优先使用别名。当前 VS Code 任务尚未使用 `T113_HOST` 环境变量，因此仅设置该变量不会自动更新任务中的目标地址。

## 部署与 gdbserver

当前 VS Code 部署任务执行：

```bash
scp build/t113_terminal root@192.168.34.238:/mnt/UDISK/t113_terminal
```

程序部署到板端后，启动任务会执行以下操作：

```bash
ssh root@192.168.34.238 \
  'chmod +x /mnt/UDISK/t113_terminal; \
   pkill gdbserver 2>/dev/null || true; \
   nohup gdbserver :2345 /mnt/UDISK/t113_terminal \
     >/tmp/gdbserver.log 2>&1 </dev/null &'
```

该操作会结束板端已有的 `gdbserver` 进程，并在 `2345` 端口后台启动新的服务。调试异常时，可在板端查看：

```bash
ssh root@"$T113_HOST" 'cat /tmp/gdbserver.log'
```

除非明确要求，不要删除远程文件、重启设备或修改系统服务。

## 修改约定

- 保持 CMake 配置与交叉编译工具链可用。
- 新增功能时优先修改 `src/`，不要把生成文件提交到源码目录。
- 修改后应至少检查 CMake 配置和本地可用的构建步骤；如果交叉编译器或目标板不可用，应明确说明未完成的验证项。
- 不要提交 SSH 私钥、密码、令牌或包含敏感信息的本地配置。

## Git 提交规范

- 每次功能、修复、重构或文档变更完成并验证后，按功能创建 Git 提交。
- 单个提交只包含一个清晰主题，不要把无关功能、临时文件或构建产物混入同一提交。
- 提交前检查 `git diff --check`，并运行与改动相关的构建或测试。
- 提交信息使用简洁的动作描述，并优先采用以下前缀：
  - `feat:`：新增功能
  - `fix:`：问题修复
  - `refactor:`：不改变外部行为的重构
  - `docs:`：文档变更
  - `test:`：测试变更
- 用户明确保留的未跟踪文件或与当前任务无关的修改不得自动加入提交。
