# rbank 跨服银行插件

使用 LeviLamina 为 MCBE 服务器开发的一个基于原版经济插件的拓展插件，实现多个服务器之间经济互通。

请先阅读下方文档，了解该插件的开发需求与官方提供的文档和公开接口。

## 相关文档

- [插件架构](./docs/插件架构.md)
- [原版经济插件仓库](./docs-ref/LegacyMoney)
- [LeviLamina 文档 nav 索引](./docs-ref/LeviLamina/docs/main/docs.config.ts)
- [LeviLamina API 文档](./docs-ref/LeviLamina/src*)

## 开发环境/工具链

使用 `./scripts/prepare-tools.ps1` 检查工具链并自动下载工具。

开发环境信息位于 `./.env-dev` 文件中。

## 源码格式规范

每次开发完成与提交时，使用 clang-format 进行格式化。

### 一行只做一件事

一条语句一行，禁止 `a(); b();` 这种压行。控制流必须带花括号，禁止 `if (x) return;`、`for (...) f();`——`.clang-format` 已开 `InsertBraces`，重排后自动补齐。

### 空行划分执行阶段

空行是语义分隔，不交给工具，靠 `.clang-format` 的 `SeparateDefinitionBlocks: Leave` 保持原样。函数体内按「守卫 → 计算 → 落库 → 返回」分段，每段一个空行：

- **前置守卫**后空行：参数校验、权限检查、实体查找。
- **计算**与**落库**之间空行：算出金额/批次后，再进事务写。
- **事务块**之间空行：多段 `Transaction` 串行执行时，每块独立。
- **返回**前空行：`return` / `throw` 与上面的逻辑隔开。

```cpp
Result Bank::deposit(CashRequest const& q) {
    auto s   = db.open({q.xuid});
    auto cfg = s->config();
    ready(*s, q.xuid);
    require(cfg.deposits, "DISABLED", "存款暂未开放");

    auto c    = owned(*s, q.xuid, q.card);
    auto r    = rate(*s);
    auto base = FeePolicy::convert(q.local, r);
    limits(*s, cfg, q.xuid, Direction::Deposit, base);

    auto fee = FeePolicy::calculate(base, cfg.deposit);
    require(fee < base, "FEE", "手续费超过本金");
    verify(c, s->lots(c.no));

    auto balance = money.get(q.xuid);
    require(balance && *balance >= q.local, "LEGACY_BALANCE", "本服余额不足或原版经济不可用");
    ...
```

### 结构与分组

- 顶层定义（函数、结构体、类）之间恰好一个空行，禁止连续两个空行。
- 类的数据成员按「配置 / 依赖 / 状态」分组，组间空行；同一组的同类单行访问器连续书写，不逐行拆开。
- 单行实现（`int x() const { return x_; }`）允许保留在一行，不要展开成四行。
- 命名空间结尾写 `} // namespace rbank::core` 形式的注释。

### 不要在源码里加无关文案

排版改动只动空白与花括号，必须保持 token 序列不变；若某处结构确实混乱到 clang-format 排不好（例如函数指针成员被断行），改用类型别名等等价写法修正结构，而不是手工对抗格式化器。
