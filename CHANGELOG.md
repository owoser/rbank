# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- 跨服银行卡：开卡、删卡、冻结、卡槽上限，卡号规则由 `config.json` 的字段 DSL 描述（服务码 / 随机 / XUID 哈希 / 序列，可选 Luhn 校验）。
- 存款、取款、银行内转账，以及基于批次的跨服手续费：同服批次取款免费，跨服批次按费率计收并销毁。
- Saga 资金协议：LegacyMoney `Reduce` / `Add` 与数据库写入之间用 `intent` 单据衔接，外部调用结果不明时不重放，由恢复线程标记 `ORPHAN` 等待人工裁决。
- 幂等：`intent.request_key` 唯一索引 + CAS 状态转移，重复提交只生效一次。
- 并发控制：按 xuid 排序的 `GET_LOCK`、事务内 `FOR UPDATE`、银行卡乐观版本号。
- 游玩端表单（卡片面板、存取款、开卡）、`/rbank` 命令树、可选 HTTP 管理 API。
- 节点登记与心跳、汇率表（含生效时间）、原版经济审计表。
- 首次启动自动建表迁移，热配置存于数据库 `config` 表。
