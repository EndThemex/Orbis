---
description: Orbis 项目通用规则
globs: 
alwaysApply: true
---

# Orbis 项目规则

## 构建与验证
- 不要主动执行编译/构建（如 `pio run`、`pio run -t upload`、`idf.py build`、`make`、`npm run build` 等）。
- 修改代码后只用文字说明改动内容、涉及文件与影响范围，由用户自行决定何时编译。
- 如确需编译验证，必须先征得用户同意再执行。
