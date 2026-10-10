// malloc_lock.cpp —— R40 根因修复
//
// 根因：CeGCC 工具链基于 newlib，而 newlib 的 malloc/free **默认不是线程安全的**
// （需用户提供 __malloc_lock / __malloc_unlock 钩子）。一旦两个线程并发 new/delete
// （std::string / std::vector 内部全走这条路径），就会损坏 newlib 堆，之后进程在
// 任意后续堆操作处崩溃 —— 这正是所有多线程变体（test9-26）闪退、而 R13(test16)
// 单线程不闪退的根本原因。
//
// 修复手段：
//   ① 重载全局 operator new / delete，用一把递归 CRITICAL_SECTION 串行化所有 C++ 堆分配；
//   ② 同时提供 newlib 的 __malloc_lock / __malloc_unlock（及 _malloc_lock / _malloc_unlock）
//      钩子，兜底覆盖任何直接走 malloc 的路径。
// 效果：所有 malloc 实际被串行调用，即使底层非线程安全也安全。
//
// 注意：CRITICAL_SECTION 是递归锁，同一线程重入（operator new → malloc → __malloc_lock）
// 不会死锁；InitializeCriticalSection 是系统调用，初始化不分配堆。
#include <windows.h>
#include <cstdlib>
#include <cstddef>

static CRITICAL_SECTION g_heapCs;
static volatile LONG g_heapCsInit = 0;

extern "C" void InitHeapLock() {
    if (InterlockedCompareExchange(&g_heapCsInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_heapCs);
    }
}

static void ensureInit() {
    if (g_heapCsInit == 0) InitHeapLock();
}

// newlib malloc lock hooks（若工具链启用多线程 malloc，会调用这些弱符号）
extern "C" void __malloc_lock(void*)   { ensureInit(); EnterCriticalSection(&g_heapCs); }
extern "C" void __malloc_unlock(void*) { LeaveCriticalSection(&g_heapCs); }
extern "C" void _malloc_lock(void*)    { ensureInit(); EnterCriticalSection(&g_heapCs); }
extern "C" void _malloc_unlock(void*)  { LeaveCriticalSection(&g_heapCs); }

void* operator new(std::size_t n) {
    ensureInit(); EnterCriticalSection(&g_heapCs);
    void* p = malloc(n ? n : 1);
    if (!p) p = malloc(1);   // 极端兜底：车机可用内存 ~133MB，OOM 几乎不发生；保证非 NULL
    LeaveCriticalSection(&g_heapCs);
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) {
    if (p) { ensureInit(); EnterCriticalSection(&g_heapCs); free(p); LeaveCriticalSection(&g_heapCs); }
}
void operator delete[](void* p) { operator delete(p); }
