#pragma once
// syssync.h - 极简跨平台同步原语（WinCE 用 Win32 API，主机测试用 pthread）。
// 仅服务于 AdbTransport 的“独立读泵线程 + 上层并发写”模型，避免引入 Win32 条件变量
// （WinCE 6.0 没有 CONDITION_VARIABLE）。
//
// ⚠️ windows.h 必须只在 _WIN32 下包含：早前它写在宏之外，导致 adb.cpp / net.cpp
// 这类【声称可在宿主机编译做验证】的代码在 POSIX 上连 <windows.h> 都找不到，
// 主机端回归测试与 CI 静态检查全部失效。
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
class SysMutex {
    CRITICAL_SECTION m_cs;
public:
    SysMutex() { InitializeCriticalSection(&m_cs); }
    ~SysMutex() { DeleteCriticalSection(&m_cs); }
    void lock() { EnterCriticalSection(&m_cs); }
    void unlock() { LeaveCriticalSection(&m_cs); }
};
class SysEvent {
    HANDLE m_h;
public:
    SysEvent() { m_h = CreateEvent(NULL, FALSE/*auto-reset*/, FALSE, NULL); }
    ~SysEvent() { if (m_h) CloseHandle(m_h); }
    void set() { if (m_h) SetEvent(m_h); }
    // 等待至多 ms 毫秒（ms<0 表示无限）；返回 true=被信号唤醒，false=超时
    bool wait(int ms) { if (!m_h) return false; return WaitForSingleObject(m_h, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0; }
};
#else
#include <pthread.h>
#include <time.h>
class SysMutex {
    pthread_mutex_t m_m = PTHREAD_MUTEX_INITIALIZER;
public:
    void lock() { pthread_mutex_lock(&m_m); }
    void unlock() { pthread_mutex_unlock(&m_m); }
};
class SysEvent {
    pthread_mutex_t m_m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  m_c = PTHREAD_COND_INITIALIZER;
    bool m_flag = false;
public:
    void set() {
        pthread_mutex_lock(&m_m);
        m_flag = true;
        pthread_cond_signal(&m_c);
        pthread_mutex_unlock(&m_m);
    }
    bool wait(int ms) {
        pthread_mutex_lock(&m_m);
        if (m_flag) { m_flag = false; pthread_mutex_unlock(&m_m); return true; }
        bool ok = true;
        if (ms < 0) {
            pthread_cond_wait(&m_c, &m_m);
        } else {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            ok = (pthread_cond_timedwait(&m_c, &m_m, &ts) == 0);
        }
        m_flag = false;
        pthread_mutex_unlock(&m_m);
        return ok;
    }
};
#endif
