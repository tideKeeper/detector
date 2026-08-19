#include "memory_detect.h"
#include <dlfcn.h>     // 用于动态链接库操作，如dladdr函数
#include <execinfo.h>  // 用于获取调用栈，如backtrace函数
#include <unistd.h>    // 系统调用，如getpagesize函数
#include <algorithm>
#include <cstddef>
#include <cstdio>   // 标准输入输出
#include <cstdlib>  // 标准库函数
#include <cstring>  // 字符串操作函数
#include <mutex>    // 互斥锁，保证线程安全
#include <ratio>
#include <string>
#include <unordered_map>     // 哈希表，用于存储内存分配信息
#include <vector>            // 动态数组
#include "output_control.h"  // 输出控制模块
#include "plthook.h"         // PLT钩子模块，用于函数替换

#define TRACKER_DEBUG(...) ((void)0)

// ======== tracker模块, 功能的大脑部分, 负责记录内存分配和释放
namespace tracker {

// 单条内存分配的信息
struct AllocationInfo {
    size_t size;          // 内存大小
    void* callstack[16];  // 调用栈信息
    int callstack_size;   // 调用栈数量
};

// 全局内存账本, 纯数据统计
// 单例模式, 所有库的分配都汇总在这里, 也就是记账的只要一个人
class MemoryTracker {
   public:
    static MemoryTracker& GetInstance() {
        static MemoryTracker instance;
        return instance;
    }

    void RecordAllocation(void* ptr, size_t size);
    void RecordDeallocation(void* ptr);
    void PrintStatus() const;
    bool HasLeaks() const;
    size_t GetTotalAllocated() const;
    size_t GetActiveAllocations() const;

   private:
    MemoryTracker() : total_allocated_(0), total_freed_(0), active_allocations_cnt_(0) {}
    MemoryTracker(const MemoryTracker&) = delete;
    MemoryTracker& operator=(const MemoryTracker&) = delete;

    mutable std::mutex mutex_;

    // 用哈希表来存储活跃分配信息, key是分配的内存地址, value是分配信息
    std::unordered_map<void*, AllocationInfo> allocations_;

    size_t total_allocated_;         // 总分配内存
    size_t total_freed_;             // 总释放内存
    size_t active_allocations_cnt_;  // 活跃分配数量
};

void MemoryTracker::RecordAllocation(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    AllocationInfo info;
    info.size = size;
    // 从这里开始, 往前回溯, 记录函数栈信息
    info.callstack_size = backtrace(info.callstack, 16);

    allocations_[ptr] = info;
    total_allocated_ += size;
    active_allocations_cnt_++;
}

void MemoryTracker::RecordDeallocation(void* ptr) {
    if (!ptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = allocations_.find(ptr);

    if (it != allocations_.end()) {
        total_freed_ += it->second.size;
        active_allocations_cnt_--;
        // 从表中删除
        allocations_.erase(it);
    }
}

void MemoryTracker::PrintStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);

    TRACKER_PRINT("\n=== Memory Tracker Status ===");
    TRACKER_PRINT("Total Allocated: %zu bytes\n", total_allocated_);
    TRACKER_PRINT("Total Freed: %zu bytes\n", total_freed_);
    TRACKER_PRINT("Active Allocations: %zu\n", active_allocations_cnt_);
    TRACKER_PRINT("Potential Leaks: %zu\n", allocations_.size());

    // 当有潜在泄漏时, 才打印详细信息
    if (!allocations_.empty()) {
        TRACKER_PRINT("\nDetailed leak information:\n");

        // 打印每个泄漏的详细信息
        for (const auto& pair : allocations_) {
            void* leak_addr = pair.first;
            const auto& info = pair.second;
            // 位置 + 大小
            // pair.first 是泄漏的内存地址
            TRACKER_PRINT("\nLeak at %p (size: %zu bytes)\n", leak_addr, info.size);

            // 将地址数组, 转换为易读的符号名
            char** symbols = backtrace_symbols(info.callstack, info.callstack_size);

            // 尝试精细化打印
            if (symbols) {
                TRACKER_PRINT("Callstack:\n");

                // 逐层解析调用栈, 需要把函数的调用栈完整地打印出来
                for (int i = 0; i < info.callstack_size; i++) {
                    // 记录绝对地址
                    void* abs_addr = info.callstack[i];

                    // 承载查询结果, 共四个字段
                    // dli_fname 地址所属模块的文件路径
                    // dli_fbase 是该模块在内存中加载的基地址
                    // dli_sname 是该符号的名称
                    // dli_saddr 是该符号的起始地址
                    Dl_info dl_info;

                    // dladdr精细化查询地址所属的模块
                    if (dladdr(abs_addr, &dl_info)) {
                        // 计算模块内相对偏移地址, 用来定位行号
                        void* rel_addr = (void*)((char*)abs_addr - (char*)dl_info.dli_fbase);
                        // 打印绝对地址和相对偏移地址
                        TRACKER_PRINT("  [%d] Absolute: %p, Relative: %p\n", i, abs_addr, rel_addr);
                        // 打印模块的文件路径
                        TRACKER_PRINT("      Module: %s\n", dl_info.dli_fname);

                        // 调用addr2line命令
                        // -e 指定模块路径, -f 显示函数名,
                        char cmd[256];
                        snprintf(cmd, sizeof(cmd), "addr2line -e %s -f -C -p %p", dl_info.dli_fname, rel_addr);

                        // 执行命令, 并读取输出
                        FILE* pipe = popen(cmd, "r");
                        if (pipe) {
                            char line[256];
                            if (fgets(line, sizeof(line), pipe)) {
                                TRACKER_PRINT("      Source: %s", line);
                            }
                            // 关闭管道
                            pclose(pipe);
                        }

                    } else {
                        // dladdr查询失败, 降级使用backtrace_symbols
                        TRACKER_PRINT("[%d] %s\n", i, symbols[i]);
                    }
                }

                // 释放符号trace_symbols分配的内存
                free(symbols);
            }
        }

        TRACKER_PRINT("\n================\n");
    }
}

bool MemoryTracker::HasLeaks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !allocations_.empty();
}

size_t MemoryTracker::GetTotalAllocated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_allocated_;
}

size_t MemoryTracker::GetActiveAllocations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_allocations_cnt_;
}

MemoryTracker& Instance() {
    return MemoryTracker::GetInstance();
}

}  // namespace tracker

//========= hooked函数模块

// 原函数指针, 静态全局变量, hook成功后赋予真正的函数地址
static void* (*orig_malloc)(size_t) = nullptr;
static void (*orig_free)(void*) = nullptr;
static void* (*orig_calloc)(size_t, size_t) = nullptr;
static void* (*orig_realloc)(void*, size_t) = nullptr;
static void* (*orig_new)(size_t) = nullptr;
static void (*orig_delete)(void*) = nullptr;
static void* (*orig_new_array)(size_t) = nullptr;
static void (*orig_delete_array)(void*) = nullptr;

static void* HookedMalloc(size_t size) {
    TRACKER_DEBUG("HookedMalloc: %zu\n", size);

    void* ptr = orig_malloc(size);

    tracker::Instance().RecordAllocation(ptr, size);

    // 返回分配的内存地址, 供使用
    return ptr;
}

static void HookedFree(void* ptr) {
    TRACKER_DEBUG("HookedFree: %p\n", ptr);

    // 释放内存
    orig_free(ptr);

    // 消账
    tracker::Instance().RecordDeallocation(ptr);
}

// calloc = 分配 + 清零，参数是「元素个数 × 每个元素大小
static void* HookedCalloc(size_t num, size_t size) {
    TRACKER_DEBUG("HookedCalloc: %zu, %zu\n", num, size);

    void* ptr = orig_calloc(num, size);
    tracker::Instance().RecordAllocation(ptr, num * size);

    return ptr;
}

// realloc = 重新分配内存，参数是「旧内存指针 × 新内存大小」
// 可能原地扩容, 可能换地址, 也可能申请失败
static void* HookedRealloc(void* old_ptr, size_t new_size) {
    TRACKER_DEBUG("HookedRealloc: %p, %zu\n", old_ptr, new_size);

    // 调用realloc
    void* new_ptr = orig_realloc(old_ptr, new_size);

    if (!new_ptr) {
        return nullptr;
    }

    // 成功, 更新记录
    if (old_ptr) {
        tracker::Instance().RecordDeallocation(old_ptr);
    }
    tracker::Instance().RecordAllocation(new_ptr, new_size);

    return new_ptr;
}

// new = 分配内存, 并初始化为0
static void* HookedOperatorNew(size_t size) {
    TRACKER_DEBUG("HookedOperateNew: %zu\n", size);

    void* ptr = orig_new(size);
    tracker::Instance().RecordAllocation(ptr, size);

    return ptr;
}

// delete = 释放内存, 并更新记录, 传入参数为空也是合法的, 不会有错误
static void HookedOperatorDelete(void* ptr) noexcept {
    TRACKER_DEBUG("HookedOperateDelete: %p\n", ptr);

    if (!ptr) {
        return;
    }

    tracker::Instance().RecordDeallocation(ptr);
    orig_delete(ptr);
}

static void* HookedOperatorNewArray(size_t size) {
    TRACKER_DEBUG("HookedOperateNewArray: %zu\n", size);

    void* ptr = orig_new_array(size);
    tracker::Instance().RecordAllocation(ptr, size);

    return ptr;
}

static void HookedOperatorDeleteArray(void* ptr) noexcept {
    TRACKER_DEBUG("HookedOperateDeleteArray: %p\n", ptr);

    if (!ptr) {
        return;
    }

    tracker::Instance().RecordDeallocation(ptr);
    orig_delete_array(ptr);
}

// memory_hook模块, 负责劫持函数

class MemoryHook {
   public:
    explicit MemoryHook(std::string lib_path) : lib_path_(std::move(lib_path)) {}

    ~MemoryHook() = default;

    // 批量替换目标库中的所有内存分配释放函数
    void Start();

   private:
    std::string lib_path_;
    std::unique_ptr<PLTHook> hook_;
};

void MemoryHook::Start() {
    // 创建PLTHook实例, 针对特定的模块, 替换所有内存分配释放函数
    hook_ = PLTHook::Create(lib_path_.c_str());
    if (!hook_) {
        TRACKER_ERROR("failed to create PLTHook instance for lib_path: %s", lib_path_.c_str());
        return;
    }

    // ---C 标准库函数
    if (hook_->ReplaceFunction("malloc", (void*)HookedMalloc, (void**)&orig_malloc) != PLTHook::SUCCESS) {
        TRACKER_WARNING("malloc not hooked: %s", PLTHook::GetLastError().c_str());
    }

    if (hook_->ReplaceFunction("free", (void*)HookedFree, (void**)&orig_free) != PLTHook::SUCCESS) {
        TRACKER_WARNING("free not hooked: %s", PLTHook::GetLastError().c_str());
    }

    if (hook_->ReplaceFunction("calloc", (void*)HookedCalloc, (void**)&orig_calloc) != PLTHook::SUCCESS) {
        TRACKER_WARNING("calloc not found in PLT, skipping");
    }

    if (hook_->ReplaceFunction("realloc", (void*)HookedRealloc, (void**)&orig_realloc) != PLTHook::SUCCESS) {
        TRACKER_WARNING("realloc not found in PLT, skipping");
    }

    // ---C++ 标准库函数
    // _Znwm = operator new(unsigned long)
    if (hook_->ReplaceFunction("_Znwm", (void*)HookedOperatorNew, (void**)&orig_new) != PLTHook::SUCCESS) {
        TRACKER_WARNING("operator new not found in PLT, skipping");
    }

    // _ZdlPv = operator delete(void*)
    if (hook_->ReplaceFunction("_ZdlPv", (void*)HookedOperatorDelete, (void**)&orig_delete) != PLTHook::SUCCESS) {
        TRACKER_WARNING("operator delete not found in PLT, skipping");
    }

    // _Znam = operator new[](unsigned long)
    if (hook_->ReplaceFunction("_Znam", (void*)HookedOperatorNewArray, (void**)&orig_new_array) != PLTHook::SUCCESS) {
        TRACKER_WARNING("operator new[] not found in PLT, skipping");
    }

    // _ZdaPv = operator delete[](void*)
    if (hook_->ReplaceFunction("_ZdaPv", (void*)HookedOperatorDeleteArray, (void**)&orig_delete_array) !=
        PLTHook::SUCCESS) {
        TRACKER_WARNING("operator delete[] not found in PLT, skipping");
    }
}

// ======== pimpl实现层

class MemoryDetectImpl {
   public:
    MemoryDetectImpl() = default;
    ~MemoryDetectImpl() = default;

    void Register(const std::string& lib_path);
    void RegisterMain();
    void Start();
    void Detect();

   private:
    std::vector<std::unique_ptr<MemoryHook>> hooks_;
};

void MemoryDetectImpl::Register(const std::string& lib_path) {
    hooks_.emplace_back(std::make_unique<MemoryHook>(lib_path));
}

void MemoryDetectImpl::RegisterMain() {
    hooks_.emplace_back(std::make_unique<MemoryHook>(std::string()));
}

void MemoryDetectImpl::Start() {
    for (auto& hook : hooks_) {
        hook->Start();
    }
}

void MemoryDetectImpl::Detect() {
    tracker::Instance().PrintStatus();
}

// ======== 外层接口完善
MemoryDetect::MemoryDetect() : impl_(std::make_unique<MemoryDetectImpl>()) {}

MemoryDetect::~MemoryDetect() = default;

void MemoryDetect::Register(const std::string& lib_path) {
    impl_->Register(lib_path);
}

void MemoryDetect::RegisterMain() {
    impl_->RegisterMain();
}

void MemoryDetect::Start() {
    impl_->Start();
}

void MemoryDetect::Detect() {
    impl_->Detect();
}
