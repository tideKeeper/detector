#include <cstdio>

extern "C" int SimpleAdd(int a, int b) {
  int sum = a + b;
  // 内部调用 printf，作为 Hook 目标
  printf("SimpleAdd: %d + %d = %d\n", a, b, sum);
  return sum;
}