# AGENTS.md — caf-plugin-development skill 维护说明

本目录是 [Agent Skills 标准](https://github.com/anthropics/skills) 结构的 skill，供各 agent 工具（Hermes / Claude Code / OpenAI Agents SDK 等）按需加载。

## 结构

```
caf-plugin-development/
├── SKILL.md       # 必需：frontmatter（name 必须等于目录名）+ 正文
├── references/    # 深潜详档（按需加载，不进主文档）
└── AGENTS.md      # 本文件
```

## 同步纪律（重要）

- **单一事实来源 = 本目录**。Hermes 侧 `~/.hermes/skills/software-development/caf-plugin-development` 是指向本目录的**符号链接**，改动直接落在这里，无需手动同步。
- 更新 SKILL.md / references/ 后，**不要**在 Hermes 侧留副本，也**不要**复制到 `docs/`（先例 hot-reload-procedure.md 的手动同步方案已被本目录 + 软链取代，历史文档保留不动）。
- 提交时连带 git 历史，回滚可追。

## 维护规则

- frontmatter `name` 与目录名必须一致；`description` 保持"何时使用"语义。
- 新排查结论优先进 references/ 详档，SKILL.md 只留要点 + 指针。
- 修改前先读文件（可能有并行会话在改）。
