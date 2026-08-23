#include <pthread.h>
#include <unistd.h>
#include <cstdio>

// 测试用全局互斥锁，定义在动态库内部
static pthread_mutex_t g_lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_b = PTHREAD_MUTEX_INITIALIZER;

// ================================================================
// 测试1：正常加解锁
// ================================================================
static void* ThreadNormalWork(void* arg) {
    printf("[Normal Test] 线程开始运行，尝试加锁...\n");
    pthread_mutex_lock(&g_lock_a);
    usleep(20 * 1000);
    pthread_mutex_unlock(&g_lock_a);
    printf("[Normal Test] 线程正常释放锁，退出\n");
    return nullptr;
}

extern "C" void StartNormalTest() {
    pthread_t t;
    pthread_create(&t, nullptr, ThreadNormalWork, nullptr);
    pthread_join(t, nullptr);
}

// ================================================================
// 测试2：经典双线程死锁
// ================================================================
static void* ThreadDeadlockA(void* arg) {
    pthread_mutex_lock(&g_lock_a);
    printf("[Deadlock Test] 线程A 已持有 lock_a，100ms后尝试获取 lock_b\n");
    usleep(100 * 1000);
    pthread_mutex_lock(&g_lock_b); // 触发死锁
    pthread_mutex_unlock(&g_lock_b);
    pthread_mutex_unlock(&g_lock_a);
    return nullptr;
}

static void* ThreadDeadlockB(void* arg) {
    pthread_mutex_lock(&g_lock_b);
    printf("[Deadlock Test] 线程B 已持有 lock_b，100ms后尝试获取 lock_a\n");
    usleep(100 * 1000);
    pthread_mutex_lock(&g_lock_a); // 触发死锁
    pthread_mutex_unlock(&g_lock_a);
    pthread_mutex_unlock(&g_lock_b);
    return nullptr;
}

extern "C" void StartDeadlockTest() {
    pthread_t t_a, t_b;
    pthread_create(&t_a, nullptr, ThreadDeadlockA, nullptr);
    pthread_create(&t_b, nullptr, ThreadDeadlockB, nullptr);
    pthread_join(t_a, nullptr);
    pthread_join(t_b, nullptr);
}

// ================================================================
// 测试3：trylock 非阻塞
// ================================================================
static void* ThreadTrylockTest(void* arg) {
    printf("[Trylock Test] 尝试非阻塞加锁...\n");
    int ret = pthread_mutex_trylock(&g_lock_a);
    if (ret == 0) {
        printf("[Trylock Test] trylock 成功，持有锁后释放\n");
        usleep(10 * 1000);
        pthread_mutex_unlock(&g_lock_a);
    } else {
        printf("[Trylock Test] trylock 失败，直接返回，无阻塞等待\n");
    }
    return nullptr;
}

extern "C" void StartTrylockTest() {
    pthread_t t;
    pthread_create(&t, nullptr, ThreadTrylockTest, nullptr);
    pthread_join(t, nullptr);
}
