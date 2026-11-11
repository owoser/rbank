# rBank 跨服银行

基于 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的 MCBE 服务器插件，在[原版经济插件 LegacyMoney](./docs-ref/LegacyMoney) 之上提供跨服经济互通：同一个玩家在多台服务器共享银行卡余额，资金在来源服与目标服之间以「批次」记录，跨服取款时才计收跨服手续费。

完整设计见 [插件架构](./docs/插件架构.md)。

## 构建

新检出先准备一次环境，两个脚本都按锁定版本下载并校验 SHA256：

```powershell
./scripts/prepare-tools.ps1   # 工具链 → .tools（xmake、LLVM、7-Zip）
./scripts/prepare-deps.ps1    # 依赖 → build/deps（LeviLamina、prelink、bedrock runtime 数据、libmariadb、各头文件库）
```

依赖清单在 `scripts/dependencies.lock.json`，每个条目带 URL 与 SHA256；导入库（`fmt.lib`、`SymbolProvider.lib`、`LeviLamina.lib`）在本地按当前工具链编译，工具链变了会自动重建。`prepare-deps.ps1 -Check` 只校验不下载。

```powershell
./scripts/build-local.ps1
```

`build-local.ps1` 与 `test-local.ps1` 开跑前都会调 `prepare-deps.ps1 -Check`，依赖缺失或与锁不一致时直接报出缺哪些，而不是让编译在头文件处报错。

产物在 `build/rbank/`：`rbank.dll`、`manifest.json`、`schema/migrations/`、`libmariadb.dll` 及其插件目录。`-NoLink` 只编译不链接，`-CompileCommandsOnly` 生成 `compile_commands.json`。

测试分两级，凭据从 `.env-dev` 的 `MYSQL_*` 读入并只注入进程环境：

```powershell
./scripts/test-local.ps1              # 纯规则，不碰 MC 与 MySQL
./scripts/test-local.ps1 -Integration # 追加真实 MySQL 的完整用例
```

集成用例会在 `MYSQL_DATABASE`（默认 `rbank_test`）里建表并清空数据，不要指向正式库。

## 多语言

界面文案随玩家客户端语言切换，语言包在 `lang/` 随插件分发。缺失的语言回落英文，缺失的键回落源码中的英文原文。HTTP 管理接口面向运维脚本，固定返回英文。

修改文案后运行 `python -I scripts/check-lang.py` 校验三份语言包的键与占位符一致。

## 部署

1. 把 `build/rbank/` 整个目录放进服务器的 `mods/rbank/`。
2. 首次启动会生成 `config/config.json` 后拒绝加载，填写数据库与节点身份：
   - `server_id` / `server_name` / `currency` 是本服在银行网络中的身份，必须与同一数据库中的其他节点互不相同，且上线后不要再改。
   - `database.*` 指向共享的 MySQL 8 实例；`library` 默认为随插件分发的 `libmariadb.dll`。
   - `card_no` 是卡号生成规则，见下。
   - `http.enabled` 打开内置管理 API 时必须提供至少 32 字符的 `token`。
3. 重启服务器，插件会在工作线程执行 `schema/migrations/*.sql` 建表与升级。

热配置（卡槽数、费率、限额、功能开关）存放在数据库 `config` 表，改完即时生效，无需重启。

## 命令

玩家命令：

```
/rbank                              卡片面板（表单）
/rbank create                       开通一张银行卡
/rbank info   <卡号>
/rbank delete <卡号>                 余额为 0 才能删卡
/rbank deposit  <卡号> <金额>        本服货币存入
/rbank withdraw <卡号> <金额>        取回本服货币
/rbank pay <卡号> <玩家或XUID> <金额> 银行内转账
/rbank list | hist | slot            卡列表 / 流水 / 卡槽使用
```

管理命令（需要 OP）：

```
/rbank list|hist|slot <XUID>
/rbank freeze|unfreeze <卡号>
/rbank rate                          查看各服汇率
/rbank rate set <server_id> <rate>   设置汇率（本服货币 / 1 base）
/rbank slot set <1-100>
```

金额一律是 base 单位：本服货币金额必须是本服汇率的整数倍，否则拒绝（不会静默截断）。

## 管理 HTTP API

`http.enabled` 为真时监听 `http.host:port`，所有请求需带 `Authorization: Bearer <token>`。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/v1/cards/{xuid}` | 玩家的卡列表 |
| GET | `/v1/ledger/{xuid}` | 最近 100 条流水 |
| GET | `/v1/nodes` | 节点与心跳 |
| GET | `/v1/rates` | 各服生效汇率 |
| GET | `/v1/config` | 当前热配置 |
| GET | `/v1/intents/{id}` | 单据状态 |
| PUT | `/v1/config` | 写入热配置 |
| POST | `/v1/cards/{card_no}/freeze` | `{"frozen": true}` |
| POST | `/v1/rates` | `{"server_id": "...", "rate": 1}` |
| POST | `/v1/intents/{id}/resolve` | `{"legacy_applied": true}`，处理 ORPHAN 单据 |

## 资金一致性

资金操作采用 Saga 而非两阶段提交，因为 LegacyMoney 只有 `Add` / `Reduce`，没有预留与回滚：

- **存款**：先写单据（`intent`），再调原版 `Reduce`，成功后才在数据库入账。原版拒绝即结束，无副作用。
- **取款**：先在数据库扣款、消耗批次并写流水，再调原版 `Add`；失败则补偿回滚银行侧。
- 原版调用结果不明（超时/崩溃）时**绝不重放**外部写入，单据停在中间状态，由恢复线程在宽限期后标记 `ORPHAN`，等人工用 `/v1/intents/{id}/resolve` 裁决。

单据以 `request_key` 唯一索引做幂等：表单每次打开生成一个 key，重复提交只会生效一次。

## 卡号规则

`card_no.fields` 按顺序拼接，`length` 必须等于各字段宽度之和：

- `SERVER_CODE:<n>`：本服 `server_code` 前缀
- `RANDOM_UPPER|RANDOM_LOWER|RANDOM_DIGIT|RANDOM_ALNUM:<n>`：从 `alphabet` 中随机取
- `XUID_HASH:<n>`：XUID 的 SHA-256 十六进制前缀
- `SEQ:<n>`：数据库自增序列，跨服唯一

`checksum` 可选 `NONE` / `LUHN` / `LUHN10`。卡号唯一性由主键保证，冲突时最多重试 5 次。

## 贡献

Issue 与 PR 都欢迎。

## 许可

见 [LICENSE](./LICENSE)。
