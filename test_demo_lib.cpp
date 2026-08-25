#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

static pthread_mutex_t g_lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_b = PTHREAD_MUTEX_INITIALIZER;

// 多线程演示专用锁（协调用 busy-wait，避免额外 mutex 污染快照）
static pthread_mutex_t g_lock_mt_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_mt_b = PTHREAD_MUTEX_INITIALIZER;

// ----------------------------------------------------------------
// 内存：正常分配并释放（.so 内）
// ----------------------------------------------------------------
extern "C" void DemoSoNormalAlloc() {
    printf("[SO Memory] malloc/calloc/realloc/free 正常配对\n");
    void* p1 = malloc(128);
    void* p2 = calloc(4, 32);
    void* p3 = realloc(p1, 256);
    free(p2);
    free(p3);

    char* buf = new char[64];
    delete[] buf;
    printf("[SO Memory] C/C++ 分配均已释放\n");
}

// ----------------------------------------------------------------
// 内存：故意泄漏（.so 内）
// ----------------------------------------------------------------
extern "C" void DemoSoMemoryLeak() {
    printf("[SO Memory] 故意泄漏 256 字节 (malloc) + 128 字节 (new[])\n");
    (void)malloc(256);
    (void)new char[128];
}

// ----------------------------------------------------------------
// 锁：正常加解锁
// ----------------------------------------------------------------
static void* ThreadNormalLock(void* /*arg*/) {
    printf("[SO Lock] 线程加锁 -> 工作 -> 解锁\n");
    pthread_mutex_lock(&g_lock_a);
    usleep(20 * 1000);
    pthread_mutex_unlock(&g_lock_a);
    return nullptr;
}

extern "C" void DemoSoNormalLock() {
    pthread_t t;
    pthread_create(&t, nullptr, ThreadNormalLock, nullptr);
    pthread_join(t, nullptr);
}

// ----------------------------------------------------------------
// 锁：trylock 非阻塞
// ----------------------------------------------------------------
static void* ThreadTrylock(void* /*arg*/) {
    int ret = pthread_mutex_trylock(&g_lock_a);
    if (ret == 0) {
        printf("[SO Lock] trylock 成功\n");
        usleep(10 * 1000);
        pthread_mutex_unlock(&g_lock_a);
    } else {
        printf("[SO Lock] trylock 失败（非阻塞返回）\n");
    }
    return nullptr;
}

extern "C" void DemoSoTrylock() {
    pthread_t t;
    pthread_create(&t, nullptr, ThreadTrylock, nullptr);
    pthread_join(t, nullptr);
}

// ----------------------------------------------------------------
// 锁：经典死锁（放最后运行，会阻塞）
// ----------------------------------------------------------------
static void* ThreadDeadlockA(void* /*arg*/) {
    pthread_mutex_lock(&g_lock_a);
    printf("[SO Deadlock] 线程 A 持有 lock_a，等待 lock_b\n");
    usleep(100 * 1000);
    pthread_mutex_lock(&g_lock_b);
    pthread_mutex_unlock(&g_lock_b);
    pthread_mutex_unlock(&g_lock_a);
    return nullptr;
}

static void* ThreadDeadlockB(void* /*arg*/) {
    pthread_mutex_lock(&g_lock_b);
    printf("[SO Deadlock] 线程 B 持有 lock_b，等待 lock_a\n");
    usleep(100 * 1000);
    pthread_mutex_lock(&g_lock_a);
    pthread_mutex_unlock(&g_lock_a);
    pthread_mutex_unlock(&g_lock_b);
    return nullptr;
}

extern "C" void DemoSoDeadlock() {
    pthread_t t_a, t_b;
    pthread_create(&t_a, nullptr, ThreadDeadlockA, nullptr);
    pthread_create(&t_b, nullptr, ThreadDeadlockB, nullptr);
    pthread_join(t_a, nullptr);
    pthread_join(t_b, nullptr);
}

// ----------------------------------------------------------------
// 多线程内存：并发分配/释放，仅 1 个线程故意泄漏
// ----------------------------------------------------------------
struct MtAllocArg {
    int id;
    int leak;
};

static void* ThreadMtAlloc(void* arg) {
    auto* a = static_cast<MtAllocArg*>(arg);
    pthread_t tid = pthread_self();

    // 先做一轮并发分配/释放，压测记账锁
    for (int i = 0; i < 32; ++i) {
        void* p = malloc(48);
        free(p);
    }

    void* leak_or_keep = malloc(64);
    printf("[SO MT Memory] 线程 %d (tid=%lu) malloc(64) %s\n", a->id, (unsigned long)tid,
           a->leak ? "——故意不释放" : "——随后释放");
    if (!a->leak) {
        free(leak_or_keep);
    }
    return nullptr;
}

extern "C" void DemoSoMtMemory() {
    constexpr int kN = 4;
    pthread_t threads[kN];
    MtAllocArg args[kN];

    printf("[SO MT Memory] 4 线程并发 malloc/free，仅线程 3 泄漏 64 字节\n");
    for (int i = 0; i < kN; ++i) {
        args[i].id = i;
        args[i].leak = (i == 3) ? 1 : 0;
        pthread_create(&threads[i], nullptr, ThreadMtAlloc, &args[i]);
    }
    for (int i = 0; i < kN; ++i) {
        pthread_join(threads[i], nullptr);
    }
}

// ----------------------------------------------------------------
// 多线程锁快照：两线程同时持有不同锁，供外部 Detect()
// ----------------------------------------------------------------
static std::atomic<int> g_mt_held{0};
static std::atomic<int> g_mt_release{0};
static pthread_t g_hold_threads[2];

static void* ThreadMtHold(void* arg) {
    auto* m = static_cast<pthread_mutex_t*>(arg);
    pthread_mutex_lock(m);
    printf("[SO MT Hold] 线程 %lu 持有锁 %p，等待外部 Detect 后再释放\n", (unsigned long)pthread_self(),
           static_cast<void*>(m));
    g_mt_held.fetch_add(1);
    while (!g_mt_release.load()) {
        usleep(1000);
    }
    pthread_mutex_unlock(m);
    printf("[SO MT Hold] 线程 %lu 已释放锁 %p\n", (unsigned long)pthread_self(), static_cast<void*>(m));
    return nullptr;
}

extern "C" void DemoSoMtLockHoldStart() {
    g_mt_held.store(0);
    g_mt_release.store(0);
    pthread_create(&g_hold_threads[0], nullptr, ThreadMtHold, &g_lock_mt_a);
    pthread_create(&g_hold_threads[1], nullptr, ThreadMtHold, &g_lock_mt_b);
    while (g_mt_held.load() < 2) {
        usleep(1000);
    }
    printf("[SO MT Hold] 两把锁均已被不同线程持有，可调用 Detect 查看快照\n");
}

extern "C" void DemoSoMtLockHoldStop() {
    g_mt_release.store(1);
    pthread_join(g_hold_threads[0], nullptr);
    pthread_join(g_hold_threads[1], nullptr);
}

// ----------------------------------------------------------------
// 多线程等待快照：一线程持有，另一线程阻塞等待同一把锁
// ----------------------------------------------------------------
static std::atomic<int> g_mt_holder_ready{0};
static pthread_t g_wait_holder;
static pthread_t g_wait_waiter;

static void* ThreadMtWaitHolder(void* /*arg*/) {
    pthread_mutex_lock(&g_lock_mt_a);
    printf("[SO MT Wait] 持有者 %lu 已拿到 lock_mt_a\n", (unsigned long)pthread_self());
    g_mt_holder_ready.store(1);
    while (!g_mt_release.load()) {
        usleep(1000);
    }
    pthread_mutex_unlock(&g_lock_mt_a);
    return nullptr;
}

static void* ThreadMtWaitWaiter(void* /*arg*/) {
    while (!g_mt_holder_ready.load()) {
        usleep(1000);
    }
    printf("[SO MT Wait] 等待者 %lu 即将阻塞在 lock_mt_a 上\n", (unsigned long)pthread_self());
    pthread_mutex_lock(&g_lock_mt_a);
    pthread_mutex_unlock(&g_lock_mt_a);
    printf("[SO MT Wait] 等待者 %lu 已获得并释放 lock_mt_a\n", (unsigned long)pthread_self());
    return nullptr;
}

extern "C" void DemoSoMtLockWaitStart() {
    g_mt_release.store(0);
    g_mt_holder_ready.store(0);
    pthread_create(&g_wait_holder, nullptr, ThreadMtWaitHolder, nullptr);
    pthread_create(&g_wait_waiter, nullptr, ThreadMtWaitWaiter, nullptr);
    while (!g_mt_holder_ready.load()) {
        usleep(1000);
    }
    // 给等待者进入 hooked lock + RecordBeforeLock 的时间
    usleep(50 * 1000);
    printf("[SO MT Wait] 持有者占锁、等待者应已进入等待，可调用 Detect\n");
}

extern "C" void DemoSoMtLockWaitStop() {
    g_mt_release.store(1);
    pthread_join(g_wait_holder, nullptr);
    pthread_join(g_wait_waiter, nullptr);
}

// ----------------------------------------------------------------
// 多线程忙碌：持续分配/加锁，供监控线程周期性 Detect
// ----------------------------------------------------------------
static std::atomic<int> g_busy_stop{0};
static pthread_t g_busy_threads[4];

static void* ThreadMtBusy(void* arg) {
    const int id = *static_cast<int*>(arg);
    while (!g_busy_stop.load()) {
        void* p = malloc(32);
        pthread_mutex_lock(&g_lock_mt_a);
        usleep(3 * 1000);
        pthread_mutex_unlock(&g_lock_mt_a);
        free(p);
        usleep(3 * 1000);
        (void)id;
    }
    return nullptr;
}

static int g_busy_ids[4] = {0, 1, 2, 3};

extern "C" void DemoSoMtBusyStart() {
    g_busy_stop.store(0);
    printf("[SO MT Busy] 启动 4 个工作线程（循环 malloc/free + 争用同一把锁）\n");
    for (int i = 0; i < 4; ++i) {
        pthread_create(&g_busy_threads[i], nullptr, ThreadMtBusy, &g_busy_ids[i]);
    }
}

extern "C" void DemoSoMtBusyStop() {
    g_busy_stop.store(1);
    for (int i = 0; i < 4; ++i) {
        pthread_join(g_busy_threads[i], nullptr);
    }
    printf("[SO MT Busy] 工作线程已全部退出\n");
}
