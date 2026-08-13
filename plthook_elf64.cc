#include "plthook.h"

#include <dlfcn.h> //dlopen(),
#include <elf.h>
#include <errno.h>  // errno
#include <limits.h> // PATH_MAX
#include <link.h>   // struct link_map
#include <sys/mman.h>
#include <unistd.h>
#include <cstdarg> // va_list, va_start, va_end
#include <cstdio>
#include <cstring>
// #include <memory>
#include <string>
#include <vector>
#include <system_error>

/*
定义重定位类型常量, 使用x86_64架构的JUMP_SLOT
用于函数调用的重定位类型
遍历整张重定位表时，只挑出 “函数跳转类” 的条目，过滤掉数据变量等其他类型的重定位
*/
constexpr auto R_JUMP_SLOT = R_X86_64_JUMP_SLOT;

// 记录内存保护的信息, 把只读的改成可写的, 使用完后再还原
struct MemoryProtection
{
    size_t start;   // 区域起址
    size_t end;     // 结束地址
    int protection; // 保护属性(PORT_READ, PORT_WRITE, PORT_EXEC)
};

// 具体实现类
struct PLTHook::Impl
{
    // 动态符号表指针, 包含了所有符号信息
    // 名称、地址、大小、类型等
    const Elf64_Sym *dynsym_;

    // 动态字符串表指针, 存储符号名称
    const char *dynstr_;

    // 动态字符串表大小
    size_t dynstr_size_;

    // plt表的起始地址
    void *plt_addr_base_;

    // 重定位表指针, 存储了符号的重定位信息
    const Elf64_Rela *rela_plt_;

    // 重定位表中条目的数量, 遍历所有的重定位项
    size_t rela_plt_cnt;

    // 内存保护信息列表
    std::vector<MemoryProtection> memory_protections_;

    //==============静态成员==============
    // 内存页大小, 通常为4KB
    static size_t page_size_; // 静态成员不能在类内初始化

    static std::string last_error_; // 最近的错误信息
    //=============静态成员===============

    // 避免隐式转换, 只能显式调用
    explicit Impl(struct link_map *lmap);

    void InitializeFromLinkMap(struct link_map *lmap);

    // 加载进程内存映射的保护信息
    // 通过/proc/self/maps获取内存区域的保护属性
    // 加载到 memory_protections_ 中
    void LoadMemoryProtections();

    int GetMemoryProtection(void *addr) const;

    // 查找动态表中的指定tag的条目
    // 静态是因为不依赖于任何成员数据
    // 是语义上的精准表达, 而不刻意为了无实例时调用
    static const Elf64_Dyn *FindDynamicEntry(const Elf64_Dyn *dyn, Elf64_Sxword tag);

    static void SetError(const char *fmt, ...);

    //
    static std::unique_ptr<PLTHook> Create(const char *file_name);

    // 枚举符号表中的所有符号
    int EnumerateSymbols(unsigned int &pos, const char *&name_out, void **&addr_out) const;

    // 替换函数实现
    int ReplaceFunction(const char *func_name, void *new_func, void **old_func_out = nullptr);
};

/*↓ ======静态实现======== ↓*/

size_t PLTHook::Impl::page_size_ = 0;
std::string PLTHook::Impl::last_error_ = "";

// 在动态段表中查找制定类型的表项
// 服务于InitialLizeFromLinkMap函数, 通过link_map获取到动态段表的首地址, 然后在动态段表中查找指定类型的表项
const Elf64_Dyn *PLTHook::Impl::FindDynamicEntry(const Elf64_Dyn *dyn, Elf64_Sxword tag)
{
    while (dyn->d_tag != DT_NULL)
    {
        if (dyn->d_tag == tag)
        {
            return dyn;
        }
        dyn++;
    }
    // 未找到制定类型的表项
    return nullptr;
}

// 设置错误信息
// 这里是可折叠列表的读取, 并且对信息规格化处理
// 底层原理和printf 类似, 通过可变参数列表来格式化字符串,只不过这里是存储到last_error_中, 方便后续获取错误信息
void PLTHook::Impl::SetError(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    last_error_ = buf;
}

/*↑ ======静态实现======== ↑*/

/*↓ ======PLT的接口完善======== ↓*/

// 构造函数, 初始化impl, 这个构造函数是被私有化的, 需要在create静态函数中调用获取
// 主要是把link_map传入impl中并进行构造, 让impl去初始化所需的表信息
PLTHook::PLTHook(struct link_map *lmap) : pimpl_(new Impl(lmap)) {}

// 析构函数, 释放impl
PLTHook::~PLTHook() = default;

// 调用时, 传入库的名称, 获得目标库的link_map, 如果为空, 则指向主程序
// 对plthook的构造函数进行调用, 完成hook实例的创建
std::unique_ptr<PLTHook> PLTHook::Create(const char *file_name)
{
    return Impl::Create(file_name);
}

std::unique_ptr<PLTHook> PLTHook::Impl::Create(const char *file_name)
{
    // filename为空
    if (!file_name)
    {
        printf("Creating PLTHook for main program\n");

        // 获取主程序的link_map

        struct link_map *lmap = nullptr;

        // 主程序肯定是已经加载了
        void *handle = dlopen(nullptr, RTLD_LAZY);

        // 获取句柄失败
        if (!handle)
        {
            printf("dlopen failed: %s\n", dlerror());
            SetError("dlopen failed: %s", dlerror());
            throw std::runtime_error(last_error_);
        }

        // dlinfo成功会返回0, 失败返回非0
        if (dlinfo(handle, RTLD_DI_LINKMAP, &lmap) != 0)
        {
            dlclose(handle);
            SetError("dlinfo failed: %s", dlerror());
            throw std::runtime_error(last_error_);
        }
        dlclose(handle);

        // 获取到主程序的link_map后, 需要遍历到最前面的link_map, 因为主程序的link_map可能是链表的最后一个节点
        // 而第一个元素则是主程序, 其在 link_map 链表里的标识方式和共享库不一样，不能像普通.so 那样 “按名字精准查询”，所以才用「找链表头部」的方式来定位它.
        {
            lmap = lmap->l_prev;
        }

        return std::unique_ptr<PLTHook>(new PLTHook(lmap));

        // 总流程: create(filename) -> link_map -> new PLTHook(lmap) -> new Impl(lmap) -> 初始化各类信息
    }

    // filename不为空, 获取指定库的link_map
    printf("Creating PLTHook for library: %s\n", file_name);
    // dlopen打开动态库, 获得一个句柄
    // 但不加载新的副本(RTLD_NOLOAD), 只获取已经加载的库的句柄
    void *handle = dlopen(file_name, RTLD_LAZY | RTLD_NOLOAD);

    if (!handle)
    {
        printf("dlopen failed: %s\n", dlerror());
        SetError("dlopen failed: %s", dlerror());
        throw std::runtime_error(last_error_);
    }

    struct link_map *lmap = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &lmap) != 0)
    {
        dlclose(handle);
        SetError("dlinfo failed: %s", dlerror());
        throw std::runtime_error(last_error_);
    }

    dlclose(handle);

    return std::unique_ptr<PLTHook>(new PLTHook(lmap));
} // create()函数

//替换函数
int PLTHook::ReplaceFunction(const char *func_name, void *new_func, void **old_func_out) {
    return pimpl_->ReplaceFunction(func_name, new_func, old_func_out);
}

int PLTHook::Impl::ReplaceFunction(const char *func_name, void *new_func, void **old_func_out) {
    //先获取原始函数地址
    void *original = dlsym(RTLD_DEFAULT, func_name);
    if (!original) {
        SetError("No such function: %s", func_name);
        return FUNCTION_NOT_FOUND;
    }

    //
    unsigned int pos = 0;
    const char *name; //常量指针, 指向固定的对象, 内容可以修改
    void **addr;

    //遍历符号, 对目标函数进行一一替换
    while (EnumerateSymbols(pos, name, addr) == SUCCESS) {
        //检查函数名是否匹配
        if (strncmp(name, func_name, strlen(func_name)) == 0 && (name[strlen(func_name)] == '\0' || name[strlen(func_name)] == '@')) {
            //匹配√

            //获取函数所在内存页的保护属性
            int protection = GetMemoryProtection(addr);
            //计算包含该地址的页的起始地址, 用于修改页的内存权限(必须以页为单位, 从页首地址开始)
            void *page_addr = reinterpret_cast<void *>(reinterpret_cast<size_t>(addr) & ~(page_size_ - 1));

            if (protection == 0) {
                SetError("Could not get memory protection at %p", page_addr);
                return INTERNAL_ERROR;
            }

            //如果文件不可写, 则临时改成可写
            //通过mprotect()函数进行修改
            if (!(protection & PROT_WRITE)) {
                if (mprotect(page_addr, page_size_, protection | PROT_WRITE) != 0) {
                    SetError("Could not change memory protection at %p: %s", page_addr, strerror(errno));
                    return INTERNAL_ERROR;
                }
            }

            if (old_func_out) {
                *old_func_out = original;
            }

            *addr = new_func;

            if (!(protection & PROT_WRITE)) {
                if (mprotect(page_addr, page_size_, protection) != 0) {
                    SetError("Could not restore memory protection at %p: %s", page_addr, strerror(errno));
                    return INTERNAL_ERROR;
                }
            }
            return SUCCESS;
        }
    }

    //未找到目标函数
    SetError("No such function: %s", func_name);
    return FUNCTION_NOT_FOUND;
}

const std::string& PLTHook::GetLastError() {
    return Impl::last_error_;
}

    /*↑ ======PLT的接口完善======== ↑*/

    // impl构造函数
    PLTHook::Impl::Impl(struct link_map *lmap)
{
    if (page_size_ == 0)
    {
        // 获取系统内存页的大小
        page_size_ = sysconf(_SC_PAGESIZE);
    }
    InitializeFromLinkMap(lmap);
    LoadMemoryProtections();
}
// 从这个构造也可以看出impl最主要的作用, 就是初始化所需的表信息, Load内存保护的信息, 具体的工作函数, 其实这些都是具体的工作函数hhh.

void PLTHook::Impl::InitializeFromLinkMap(struct link_map *lmap)
{
    // 设置基地址, 这是共享库加载到内存中的基地址
    // lmap是一个综合性的指针, 需要对里面的内容解引用, 这里就是用plt_addr_base_来存储基地址(l_addr)
    plt_addr_base_ = reinterpret_cast<void *>(lmap->l_addr);

    // 用dyn作临时变量, 当FindDynamicEntry()成功后再对动态表中的各类符号指针进行赋值
    // 获取动态段中的符号表
    const auto *dyn = FindDynamicEntry(lmap->l_ld, DT_SYMTAB);
    if (!dyn)
    {
        SetError("Failed to find dynamic symbol table (DT_SYMTAB)");
        throw std::runtime_error(last_error_);
    }
    dynsym_ = reinterpret_cast<const Elf64_Sym *>(dyn->d_un.d_ptr);

    // 获取动态段中的字符串表
    dyn = FindDynamicEntry(lmap->l_ld, DT_STRTAB);
    if (!dyn)
    {
        SetError("Failed to find dynamic string table (DT_STRTAB)");
        throw std::runtime_error(last_error_);
    }
    dynstr_ = reinterpret_cast<const char *>(dyn->d_un.d_ptr);

    // 获取动态段中的字符串表大小
    dyn = FindDynamicEntry(lmap->l_ld, DT_STRSZ);
    if (!dyn)
    {
        SetError("Failed to find dynamic string table size (DT_STRSZ)");
        throw std::runtime_error(last_error_);
    }
    dynstr_size_ = static_cast<size_t>(dyn->d_un.d_val);

    // 获取动态段中的重定位表
    dyn = FindDynamicEntry(lmap->l_ld, DT_JMPREL);
    if (!dyn)
    {
        SetError("Failed to find relocation table (DT_JMPREL)");
        throw std::runtime_error(last_error_);
    }
    rela_plt_ = reinterpret_cast<const Elf64_Rela *>(dyn->d_un.d_ptr);

    // 获取plt表条目数量, 总大小 / 单位条目大小
    dyn = FindDynamicEntry(lmap->l_ld, DT_PLTRELSZ);
    if (!dyn)
    {
        SetError("Failed to find relocation table size (DT_PLTRELSZ)");
        throw std::runtime_error(last_error_);
    }
    rela_plt_cnt = static_cast<size_t>(dyn->d_un.d_val) / sizeof(Elf64_Rela);
} // InitializeFromLinkMap()函数

// 加载内存保护信息, 通过/proc/self/maps获取内存区域的保护属性
void PLTHook::Impl::LoadMemoryProtections()
{
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp)
    {
        SetError("Failed to open /proc/self/maps");
        throw std::system_error(errno, std::system_category(), last_error_);
    }

    //用buf缓冲区存储每一行的内容
    char buf[PATH_MAX];

    //
    while (fgets(buf, PATH_MAX, fp)) {
        unsigned long start, end;
        char perms[5]; // e.g., "r-xp"

        //获取内存的起始地址, 结束地址, 以及权限属性
        //跳过不符合格式的行, 只处理符合格式的行
        if (sscanf(buf, "%lx-%lx %4s", &start, &end, perms) != 3) {
            continue; 
        }

        //解析权限字符perms为保护标志
        int protection = 0;
        if (perms[0] == 'r') {
            protection |= PROT_READ;
        }
        if (perms[1] == 'w') {
            protection |= PROT_WRITE;
        }
        if (perms[2] == 'x') {
            protection |= PROT_EXEC;
        }

        //将内存保护信息存储到memory_protections_向量中
        memory_protections_.push_back({start, end, protection});
    }
    fclose(fp);
}

//获取memory_protections_向量中的内存保护属性
int PLTHook::Impl::GetMemoryProtection(void* addr) const {
    auto addr_val = reinterpret_cast<size_t>(addr);
    for (const auto& prot : memory_protections_) {
        if (addr_val >= prot.start && addr_val < prot.end) {
            return prot.protection;
        }
    }
    return 0; // 未找到对应的内存保护属性
}

//枚举SymTable中的所有符号
//后缀加out说明是要传递出去的
int PLTHook::Impl::EnumerateSymbols(unsigned int &pos, const char *&name_out, void **&addr_out) const
{
    //遍历rela_plt_cnt次
    while (pos < rela_plt_cnt) {
        const auto *plt = &rela_plt_[pos];
        //筛选出JUMP_SLOT类型的重定位条目, 只处理函数跳转类的条目
        if (ELF64_R_TYPE(plt->r_info) == R_JUMP_SLOT) {
            //获取符号索引
            size_t idx = ELF64_R_SYM(plt->r_info);
            //获取符号名称
            name_out = dynstr_ + dynsym_[idx].st_name;
            //获取符号地址
            addr_out = reinterpret_cast<void **>(reinterpret_cast<char *>(plt_addr_base_) + plt->r_offset);
            pos++;
            return SUCCESS;
        }
        pos++;
    }
    name_out = nullptr;
    addr_out = nullptr;
    return EOF_REACHED;
}


    