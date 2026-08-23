#include "Async/AsyncExecutorNoThrow.h"

// 显式实例化默认错误类型（CTaskError）的实例，供常规使用；
// 自定义 TError 时由调用点按需隐式实例化（模板定义位于头文件）。
template class common::nothrow::CAsyncExecutor<common::nothrow::CTaskError>;
