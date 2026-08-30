# 序列化（Common/Serialization）— 实现文档

> 配套使用文档：[serialization-usage.md](serialization-usage.md)
> 源码：`Common/Serialization/BinaryReader.*`、`BinaryWriter.*`

## 1. CBinaryWriter（小端写入）

- 内部 `std::string m_buffer`；
- `WriteU8`：`push_back`；`WriteU16/32/64`：按 `value >> (i*8) & 0xFF` 循环取低字节 append（**小端**）；
- `WriteBool`：写 1 字节 0/1；`WriteBytes`：先 `WriteU32(长度)` 再 append；`WriteString` = `WriteBytes(data, size)`；
- `Size/Empty/Data`（借用 const 引用）/ `Take`（`swap` 移出）/ `Clear`。

## 2. CBinaryReader（小端，带边界检查）

内部：`const char* m_pData; size_t m_nSize; size_t m_nPos; bool m_bFailed;`（构造借用指针/引用，`nullptr` 兜底为 `""`）。

核心 `ReadRaw`：

```cpp
if (m_bFailed || m_nSize - m_nPos < nLen) { m_bFailed = true; return false; }  // 位置不动
memcpy(out, m_pData + m_nPos, nLen); m_nPos += nLen;
```

- `ReadU8/16/32/64`：经 `ReadRaw` 后按小端 `|= byte << (i*8)` 拼装；输出指针为 `nullptr` 直接置 `m_bFailed`；
- `ReadBytes/ReadString`：先 `ReadU32` 取长度，校验 `nLen > m_nSize - m_nPos` 失败；成功 `assign` 并推进；
- **失败粘滞**：`Failed()` 一旦置位，后续 `ReadRaw` / `Remaining`(=0) / `AtEnd`(=true) 恒失败，调用方据此丢弃整条消息；
- `Position/Remaining/AtEnd` 供进度查询。

## 3. 设计要点

- **借用而非持有**：Reader 不拷贝输入缓冲（零拷贝）；约束「读取期间缓冲必须存活」；
- **失败即停**：边界失败后整个 reader 失效（粘滞），避免半个字段拼出脏数据；
- **长度前缀防歧义**：字符串/字节串统一 `uint32` 长度前缀，空串合法（长度 0）。
