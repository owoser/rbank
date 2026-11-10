// 让本插件默认使用 LeviLamina 的内存分配器：跨模块传递的容器与字符串因此可以在任一侧释放。
#define LL_MEMORY_OPERATORS

#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep
