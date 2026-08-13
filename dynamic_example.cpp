#include <cstdio>

// 用 extern "C" 避免 C++ 名字修饰，保证 dlsym 能按 "SimpleAdd" 找到函数
extern "C" int SimpleAdd(int a, int b)
{
    int sum = a + b;
    // 内部调用 printf，这就是我们要 Hook 的目标调用点
    printf("SimpleAdd: %d + %d = %d\n", a, b, sum);
    return sum;
}