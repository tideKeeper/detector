#include "memory_detect.h"
#include "output_control.h" // 输出控制模块
#include "plthook.h"        // PLT钩子模块，用于函数替换
#include <cstdio>           // 标准输入输出
#include <cstdlib>          // 标准库函数
#include <cstring>          // 字符串操作函数
#include <dlfcn.h>          // 用于动态链接库操作，如dladdr函数
#include <execinfo.h>       // 用于获取调用栈，如backtrace函数
#include <mutex>            // 互斥锁，保证线程安全
#include <ratio>
#include <unistd.h>      // 系统调用，如getpagesize函数
#include <unordered_map> // 哈希表，用于存储内存分配信息
#include <vector>        // 动态数组

#define TRACKER_DEBUG(...) ((void)0)

namespace tracker {

// ======数据记录核心 memorytracker======

// 单条内存分配的信息
struct AllocationInfo {
  size_t size;         // 内存大小
  void *callstack[16]; // 调用栈信息
  int callstack_size;  // 调用栈数量
};

// 全局内存账本, 纯数据统计
// 单例模式, 所有库的分配都汇总在这里
class MemoryTracker {
public:
  static MemoryTracker &GetInstance() {
    static MemoryTracker instance;
    return instance;
  }

  void RecordAllocation(void *ptr, size_t size);
  void RecordDeallocation(void *ptr);
  void PrintStatus() const;
  bool HasLeaks() const;
  size_t GetTotalAllocated() const;
  size_t GetActiveAllocations() const;

private:
  MemoryTracker() : total_allocated(0), total_freed_(0), allocations_cnt(0) {}
  MemoryTracker(const MemoryTracker &) = delete;
  MemoryTracker &operator=(const MemoryTracker &) = delete;

  mutable std::mutex mutex_;

  // 用哈希表来存储活跃分配信息, key是分配的内存地址, value是分配信息
  std::unordered_map<void *, AllocationInfo> allocations_;

  size_t total_allocated; // 总分配内存
  size_t total_freed_;    // 总释放内存
  size_t allocations_cnt; // 活跃分配数量
};

void MemoryTracker::RecordAllocation(void *ptr, size_t size) {
  if (!ptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  AllocationInfo info;
  info.size = size;
  // 从这里开始, 往前回溯, 记录函数栈信息
  info.callstack_size = backtrace(info.callstack, 16);

  allocations_[ptr] = info;
  total_allocated += size;
  allocations_cnt++;
}

void MemoryTracker::RecordDeallocation(void *ptr) {
  if (!ptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = allocations_.find(ptr);

  if (it != allocations_.end()) {
    total_freed_ += it->second.size;
    allocations_cnt--;
    // 从表中删除
    allocations_.erase(it);
  }
}

void MemoryTracker::PrintStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);

  TRACKER_PRINT("Total Allocated: %zu bytes\n", total_allocated);
  TRACKER_PRINT("Total Freed: %zu bytes\n", total_freed_);
  TRACKER_PRINT("Active Allocations: %zu\n", allocations_cnt);
  TRACKER_PRINT("Potential Leaks: %zu\n", allocations_.size());

  if (!allocations_.empty()) {
    TRACKER_PRINT("\nDetailed leak information:\n");
  }
}

} // namespace tracker