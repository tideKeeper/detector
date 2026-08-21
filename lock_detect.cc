#include "lock_detect.h"
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>  // 线程与互斥锁标准头文件
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "output_control.h"
#include "plthook.h"

#define LOCKER_DEBUG(...) ((void)0)

namespace tracker {

// 单把锁的详细信息
// 记录持有者, 获取位置, 锁状态
struct LockInfo {
    void* lock_addr = nullptr;
    pthread_t owner_thread = 0;
    void* callstack[16];  // 获取锁调用栈, 用于定位
    int callstack_size = 0;
    bool acquired = false;  // 是否被获取
};

// 单线程的锁状态
// 记录线程持有的锁, 和正在等待的锁
// 一个线程可以持有多个锁, 但是只能等待一个锁
struct ThreadLockInfo {
    std::vector<void*> held_locks;
    void* waiting_lock = nullptr;
};

// 记账器, 供hook函数调用, 追踪, 分析
class LockTracker {
   public:
    static LockTracker& GetInstance() {
        static LockTracker instance;
        return instance;
    }

    LockTracker(const LockTracker&) = delete;
    LockTracker& operator=(const LockTracker&) = delete;

    // 记录「线程即将尝试获取锁」（调用lock之前执行）
    // 更新等待关系, 并执行死锁检测逻辑, 如果获取这把锁会形成死锁, 则立刻报告, 并且定位, 避免死锁发生
    void RecordBeforeLock(pthread_mutex_t* mutex);

    // 记录「线程成功获取锁」（调用lock之后执行）
    // 更新锁的状态, 线程锁的持有状态, 并更新等待关系
    void RecordAfterLock(pthread_mutex_t* mutex);

    // 记录「线程释放锁」（调用unlock之后执行）
    void RecordUnlock(pthread_mutex_t* mutex);

    void PrintStatus();

   private:
    // 因为记账器是单例, 所以需要互斥锁, 避免并发访问
    mutable std::mutex mutex_;
    std::unordered_map<void*, LockInfo> active_locks_;
    std::unordered_map<pthread_t, ThreadLockInfo> thread_LockInfos_;

    // 基于「线程-锁」资源分配图，深度优先搜索检测循环等待
    // 传入在等待的线程, 和目标锁: 这个线程得到了这个锁会咋样?
    void CheckDeadlock(pthread_t wait_thread, void* target_lock);

    // DFS递归检测死锁
    // current_thread 是当前追溯的线程
    // visited_threads 是已经访问过的线程(用于检测环路)
    // chain 追查出的死锁链条(线程id + 锁地址), 用于输出报告
    bool DeadlockDFS(pthread_t current_thread,
                     std::unordered_set<pthread_t>& visited_threads,
                     std::vector<std::pair<pthread_t, void*>>& chain);

    // 打印单把锁的详细信息
    void PrintLockDetail(const LockInfo& lock_info) const;

    // 打印调用栈
    void PrintCallstack(const void* callstack[], int callstack_size) const;

    LockTracker() = default;
    ~LockTracker() = default;
};

// -------- 成员函数实现

// 记录「线程即将尝试获取锁」（调用lock之前执行）
void LockTracker::RecordBeforeLock(pthread_mutex_t* mutex) {
    if (!mutex) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    void* lock_addr = static_cast<void*>(mutex);  // 获取锁地址
    pthread_t cur_thread = pthread_self();        // 获取当前线程id

    ThreadLockInfo& thread_lockinfo = thread_LockInfos_[cur_thread];

    thread_lockinfo.waiting_lock = lock_addr;

    auto lock_it = active_locks_.find(lock_addr);
    // 如果锁已被获取, 且为被持有状态, 进行死锁检测
    if (lock_it != active_locks_.end() && lock_it->second.acquired) {
        CheckDeadlock(cur_thread, lock_addr);
    }
}

// 记录「线程成功获取锁」（调用lock之后执行), 这里是确认不会发生死锁后进行的
void LockTracker::RecordAfterLock(pthread_mutex_t* mutex) {
    if (!mutex) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    void* lock_addr = static_cast<void*>(mutex);
    pthread_t cur_thread = pthread_self();

    // 更新锁信息
    LockInfo& lock_info = active_locks_[lock_addr];
    lock_info.lock_addr = lock_addr;
    lock_info.owner_thread = cur_thread;
    lock_info.acquired = true;
    lock_info.callstack_size = backtrace(lock_info.callstack, 16);

    // 更新线程信息
    ThreadLockInfo& thread_lockinfo = thread_LockInfos_[cur_thread];
    thread_lockinfo.held_locks.push_back(lock_addr);
    thread_lockinfo.waiting_lock = nullptr;
}

// 记录「线程释放锁」（调用unlock之后执行）
void LockTracker::RecordUnlock(pthread_mutex_t* mutex) {
    if (!mutex) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    void* lock_addr = static_cast<void*>(mutex);
    pthread_t cur_thread = pthread_self();

    // 从在职锁中移除
    active_locks_.erase(lock_addr);

    // 从线程持有列表中移除
    auto thread_it = thread_LockInfos_.find(cur_thread);
    if (thread_it != thread_LockInfos_.end()) {
        auto& held_locks = thread_it->second.held_locks;

        held_locks.erase(std::remove(held_locks.begin(), held_locks.end(), lock_addr), held_locks.end());

        // 线程没有其他锁, 且也没有等待锁, 则线程状态为「空闲」
        if (held_locks.empty() && !thread_it->second.waiting_lock) {
            thread_LockInfos_.erase(thread_it);
        }
    }
}

void LockTracker::CheckDeadlock(pthread_t wait_thread, void* target_lock) {
    // 已访问过的线程(用于检测环路)
    std::unordered_set<pthread_t> visited;

    // 追查出的死锁链条(线程id + 锁地址), 用于输出报告
    std::vector<std::pair<pthread_t, void*>> deadlock_chain;

    // 从目标锁的持有者线程开始追溯
    auto lock_it = active_locks_.find(target_lock);
    if (lock_it == active_locks_.end()) {
        return;
    }
    pthread_t owner_thread = lock_it->second.owner_thread;

    deadlock_chain.emplace_back(wait_thread, target_lock);
    visited.insert(wait_thread);

    // 开始DFS追溯
    if (DeadlockDFS(owner_thread, visited, deadlock_chain)) {
        // 检测到死锁
        TRACKER_PRINT("\n========================================\n");
        TRACKER_PRINT("⚠️  Potential Deadlock Detected!\n");
        TRACKER_PRINT("========================================\n");
        TRACKER_PRINT("Deadlock chain (thread -> waiting for lock):\n\n");

        // 打印死锁链条
        for (size_t i = 0; i < deadlock_chain.size(); i++) {
            pthread_t thread_id = deadlock_chain[i].first;
            void* wait_lock = deadlock_chain[i].second;

            TRACKER_PRINT("[Thread %lu] waiting for lock %p\n", thread_id, wait_lock);    
            TRACKER_PRINT("  This lock is held by:\n");

            auto lock_it = active_locks_.find(wait_lock);
            if (lock_it != active_locks_.end()) {
                PrintLockDetail(lock_it->second);
            }
            TRACKER_PRINT("\n");
        }
        TRACKER_PRINT("========================================\n\n");
    }
}

// 函数会递归调用
bool LockTracker::DeadlockDFS(pthread_t current_thread, std::unordered_set<pthread_t>& visited_threads, std::vector<std::pair<pthread_t, void*>>& chain) {
    // 如果当前线程已被访问, 则说明存在环路
    if (visited_threads.find(current_thread) != visited_threads.end()) {
        return true;
    }
    // 标记当前线程为已访问
    visited_threads.insert(current_thread);

    // 获取当前线程等待的锁
    // 我要的锁你用完没? 你在干嘛? 等待别的锁吗? 没有等啊, 在忙, 那没事了. 不是, 什么? 你在等我在用的锁? 可是我在等你的啊!
    auto thread_it = thread_LockInfos_.find(current_thread);
    if (thread_it == thread_LockInfos_.end()) {
        return false;
    }
    void* waiting_lock = thread_it->second.waiting_lock;
    if (!waiting_lock)  {
        return false;
    }

    chain.emplace_back(current_thread, 
    
}

}  // namespace tracker