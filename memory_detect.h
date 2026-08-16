#pragma once

#include <memory>
#include <string>

//前向声明impl类
class MemoryDetectImpl;

//内存检测类
class MemoryDetect {
public:
    // 单例模式, 提供全局访问点
    static MemoryDetect& GetInstance();


    ~MemoryDetect();

private:
    // 私有构造函数, 实现单例模式
    MemoryDetect();

    std::unique_ptr<MemoryDetectImpl> impl_;
};

