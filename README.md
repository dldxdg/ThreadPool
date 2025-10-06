### ThreadPool.hpp

**核心特性：**
- ✅ 单头文件，轻松集成
- ✅ 完全兼容C++11/17/20
- ✅ 使用条件编译自动选择`std::result_of`(C++11)或`std::invoke_result`(C++17+)
- ✅ 无任何废弃警告
- ✅ 支持有界/无界任务队列
- ✅ 异常安全保证
- ✅ 优雅关闭和立即关闭
- ✅ 返回`std::future`的任务提交

**基本用法：**

```cpp
#include <iostream>
#include "xdgThreadPool.hpp"

int main(int argc, char *argv[]) {

    xdg::ThreadPool pool; // 创建ThreadPool对象，默认根据CPU核数创建线程和队列大小
    pool.start(); // 启动线程池

    // 提交无参、无返回值任务
    pool.submit([]() {
        std::cout << "无参无返回值任务" << std::endl;
    });

    // 提交有参、无返回值任务
    pool.submit([](int x) {
        std::cout << "传来的参数：" << x << std::endl;
    }, 100);

    // 提交有参、有返回值任务
    auto result = pool.submitTask([](int x) {
        std::cout << "传来的参数：" << x << std::endl;
        return x * x;
    }, 100);
    std::cout << "返回值：" << result.get() << std::endl; // 使用get()获取返回值
    
    // 异常处理
	pool.submit([]() {
    	throw std::runtime_error("异常测试"); // 抛出异常
	});

    pool.destroy(); // 销毁线程池,默认销毁时等待所有任务完成
    
    return 0;
}
```



## 安全性保证

- ✅ **线程安全**：所有公共接口都是线程安全的
- ✅ **异常安全**：强异常保证，资源不泄漏
- ✅ **无数据竞争**：通过Valgrind/TSan验证
- ✅ **无死锁**：经过大量并发测试
- ✅ **无内存泄漏**：Valgrind检测无泄漏



## 📖 API文档

### 构造函数

```cpp
ThreadPool(unsigned int threadCount = 0, int queueSize = 0);
```

- `threadCount=0`: CPU核心数 * 2
- `queueSize=0`: 队列容量=CPU核心数 * 10
- `queueSize=-1`: 无界队列

### 方法

```cpp
int start();                          // 启动线程池
void destroy(bool immediate = false); // 停止线程池

// 提交任务并返回future
template<typename F, typename... Args>
auto submitTask(F&& f, Args&&... args) 
    -> std::future<result_type<F, Args...>>;

// 提交任务（不返回future）
template<typename F, typename... Args>
void submit(F&& f, Args&&... args);

// 获取状态
unsigned int getThreadCount() const;
int getQueueSize() const;
int getStatus() const;  // 0=运行, -1=停止
```



