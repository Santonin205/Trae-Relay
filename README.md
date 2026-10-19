# Trae Relay

Windows 原生的 Trae 本地 API 转接工具，支持通过网页登录或读取本地 Trae 登录凭据，将 Trae 账号提供为 OpenAI 兼容接口。单个 EXE 即可运行，无需 Python、Node.js 或 Docker。

目前主要验证 Traecode CN，Trae Work 请自行测试，国际版大概率用不了。

球球大家点个star吧！球球啦！

## 预览

### 运行总览

![Trae Relay 运行总览](docs/images/%E9%A2%84%E8%A7%88.png)

### 凭据管理

![Trae Relay 凭据管理](docs/images/%E5%87%AD%E6%8D%AE%E7%AE%A1%E7%90%86%E9%A2%84%E8%A7%88.png)

## 功能

- Chat Completions、Responses、模型列表
- JSON 和 SSE 流式输出
- 函数工具、Responses 自定义工具、命名空间工具
- 多轮会话、思考档位、Max 模式
- 账号发现、令牌刷新、积分和使用记录
- 网页登录与本地目录凭据切换
- 网页登录账号的保存、重命名、启用、删除和积分刷新
- 排队状态、今日 Token 和今日积分消耗
- 模型目录启动拉取，成功后每小时刷新，失败后每 5 分钟重试
- 日志开关与请求失败原因记录
- 托盘、开机自启动、自动签到、局域网访问

## 使用环境

- Windows 10/11 x64
- 可用的 Trae 账号；网页登录方式无需安装 Trae，默认目录方式需当前 Windows 用户已安装并登录 Trae
- 能够连接 Trae 上游服务

## 快速开始

1. 下载 Release 中的 `TraeRelay.exe`，或按“从源码构建”编译。
2. 双击运行。首次启动会在 EXE 同目录生成 `config.json` 和 API Key，并监听 `8317` 端口。
3. 在“凭据管理”选择登录方式，按下方说明加载或添加账号。
4. 在“运行总览”确认账号和模型已加载，然后在客户端填写：

| 配置项 | 内容 |
| --- | --- |
| Base URL | `http://127.0.0.1:8317/v1` |
| API Key | “运行总览”中的本地密钥 |
| 模型 | `/v1/models` 返回的 `id` |

### 登录方式

| 方式 | 操作 |
| --- | --- |
| 网页登录 | 选择“使用网页登录凭据”，点击“网页登录”，在浏览器完成登录后，返回列表点击该账号的“启用” |
| 本地目录 | 先在 Trae 客户端登录，再选择“加载默认目录凭据”，读取当前 Windows 用户的本地登录信息 |

网页登录支持保存多个账号，可在列表中修改名称、刷新积分或删除账号。同一时间启用一个账号，点击其他账号的“启用”即可切换；有请求进行时，请等待请求结束后再切换。

### 企业 SaaS 账号

点击同一个“网页登录”按钮，在官方页面选择“企业账号登录”。授权后自动识别企业身份，保存企业名称、企业 ID 和凭据；启用后使用企业服务获取模型和处理请求。当前适配国内公共 SaaS（`console.enterprise.trae.cn`），不支持 VPC 或私有部署。

企业账号支持令牌刷新、Chat Completions、Responses、流式输出及函数工具。个人积分与企业金额分开处理：企业运行总览显示已确认的套餐金额额度和周期，“企业用量”打开官方控制台查看实际消费和按量限额。消费明细接口需要浏览器会话，客户端授权令牌不能直接查询，因此程序不会把未知余额显示为零，也不会通过账号总消费差额推算单次请求费用。企业使用记录保留 Token，费用标注“金额未归因”。企业账号不执行个人积分签到，不显示个人会员积分倍率。

### 连接与退出

默认关闭窗口会最小化到托盘；完全退出请在托盘菜单中选择“退出”。

健康检查：`GET http://127.0.0.1:8317/health`，无需密钥。

API 支持 `Authorization: Bearer <key>` 和 `X-API-Key: <key>`。开启“任意 Key”会跳过鉴权，局域网使用时不建议开启。

## API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/health` | 健康检查，无需鉴权 |
| GET | `/v1/models` | 当前账号可用模型 |
| POST | `/v1/chat/completions` | Chat Completions |
| POST | `/v1/responses` | Responses |
| GET | `/v1/status` | 服务和账号状态 |
| OPTIONS | `/v1/*` | 浏览器跨域预检 |

## 兼容范围

- 支持文本、多轮对话、图片输入（能力取决于上游模型）、工具调用和 SSE。
- Chat Completions 支持 `reasoning_content`；Responses 支持 reasoning summary 和 `previous_response_id`。
- `stop` 由本地截断正文，最多 4 条；上游仍可能继续消耗额度。
- `previous_response_id` 默认只保留最近 64 个响应，重启或淘汰后会返回 `previous_response_not_found`。
- 可通过 `responses.sessionCachePersist` 持久化文本和工具历史，图片不会保存。

以下功能当前不提供或不保证语义一致：

- 严格 JSON Schema 结构化输出
- `temperature`、`top_p`、`seed`、`n`、`max_tokens`、`max_completion_tokens`、`max_output_tokens` 控制
- `background` 后台任务及响应查询、取消接口
- OpenAI 托管的 web search、file search、code interpreter
- `input_file`/PDF、音频、Files、Embeddings、Images、Realtime、Batch 等接口

部分未知字段会被忽略；依赖严格参数语义的客户端请先验证。

## 配置

配置文件：EXE 同目录的 `config.json`。首次运行自动生成，也可以使用 `--config` 指定路径。

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `service.port` | `8317` | HTTP 端口 |
| `service.allowLan` | `false` | 开启后监听 `0.0.0.0` |
| `service.allowAnyApiKey` | `false` | 跳过 API Key 校验 |
| `service.maxConcurrentPerAccount` | `2` | 每账号并发数，范围 1～8 |
| `responses.sessionCacheSize` | `64` | Responses 会话缓存数量 |
| `responses.sessionCachePersist` | `false` | 是否保存会话到磁盘 |
| `logging.level` | `info` | 日志级别 |
| `logging.enabled` | `true` | 开启日志，可在偏好设置中即时切换 |

开启局域网访问后，其他设备使用 `http://电脑IP:8317/v1`，并放行 Windows 防火墙端口。服务只提供 HTTP，请在可信网络中使用。

## 命令行

```powershell
.\TraeRelay.exe                 # 启动图形界面和 API
.\TraeRelay.exe --tray         # 启动后常驻托盘
.\TraeRelay.exe --serve        # 无图形界面运行 API
.\TraeRelay.exe --config path  # 指定配置文件
.\TraeRelay.exe --version      # 查看版本
```

`--serve` 不是 Windows 系统服务；默认启用单实例。

## 从源码构建

需要 Visual Studio 2022（桌面 C++）、Windows SDK 和 CMake 3.20+。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

产物：`dist/TraeRelay.exe`。全量重建：

```powershell
Remove-Item -LiteralPath .\build -Recurse
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

## 仓库内容

```text
src/                    C++ 源码、资源和版本模板
include/                头文件
docs/images/            运行总览与凭据管理截图
CMakeLists.txt          CMake 构建配置
README.md               项目说明
```

## 常见问题

| 现象 | 处理方式 |
| --- | --- |
| 没有发现账号 | 在“凭据管理”检查来源；默认目录方式需先登录 Trae，网页登录方式需完成登录并启用账号 |
| 网页登录成功后仍无可用账号 | 返回“凭据管理”，点击该账号的“启用” |
| 模型不可用 | 确认 Trae 账号权限和模型 ID，并查看 `logs/` |
| 401 | 使用“运行总览”中的 API Key |
| 503 | 检查是否已启用可用账号、并发数是否已满 |
| 端口启动失败 | 更换端口或关闭占用端口的程序 |
| 关闭窗口后仍在运行 | 程序默认最小化到托盘，在托盘菜单选择“退出” |

## 本地数据

| 路径 | 内容 |
| --- | --- |
| `config.json` | 配置和 API Key |
| `auth/` | 账号列表、凭据来源和加密保存的登录凭据 |
| `logs/` | 日志 |
| `usage/` | 使用记录 |
| `responses_sessions.json` | 开启持久化后的会话缓存 |

登录凭据使用 Windows DPAPI 加密，绑定保存时的 Windows 用户。更换电脑或 Windows 用户后，请重新登录。

