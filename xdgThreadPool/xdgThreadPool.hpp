#ifndef _XDG_THREAD_POOL_HPP_
#define _XDG_THREAD_POOL_HPP_

#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <type_traits>
#include <utility>
#include <memory>
#include <stdexcept>

namespace xdg {

/**
 * C++11/17兼容的返回类型萃取
 * 
 * 问题背景:
 * - std::result_of 在C++17中被标记为deprecated
 * - std::result_of 在C++20中被完全移除
 * - std::invoke_result 是C++17引入的替代品
 * 
 * 解决方案:
 * - 使用条件编译在编译期选择正确的类型萃取工具
 * - C++11/14: 使用std::result_of
 * - C++17+: 使用std::invoke_result
 * 
 * 这样可以保证:
 * - C++11代码正常编译(使用result_of)
 * - C++17+代码无废弃警告(使用invoke_result)
 */
#if __cplusplus >= 201703L
    // C++17及以上: 使用invoke_result避免废弃警告
    template<typename F, typename... Args>
    using result_type = typename std::invoke_result<F, Args...>::type;
#else
    // C++11/14: 使用result_of(虽然在C++17+会有警告,但C++11必需)
    template<typename F, typename... Args>
    using result_type = typename std::result_of<F(Args...)>::type;
#endif

/**
 * @brief 高性能C++线程池
 * 
 * 特性:
 * - 纯C++11实现,同时兼容C++17+(无废弃警告)
 * - 支持有界/无界任务队列
 * - 异常安全保证
 * - 优雅关闭和立即关闭
 * - 支持返回future的任务提交
 * 
 * 线程安全:
 * - 所有公共接口都是线程安全的
 * - 使用mutex保护共享状态
 * - 使用condition_variable进行线程同步
 */
class ThreadPool {
public:
    /**
     * @brief 构造函数
     * @param threadCount 线程数量,0 表示使用CPU核心数 * 2
     * @param queueSize 任务队列容量,-1表示无界队列,0表示线CPU核心数 * 10
     * 
     * 初始化策略:
     * - threadCount=0: 使用CPU核数*2作为线程数
     * - queueSize=0: 使用CPU核数*10作为队列容量
     * - queueSize=-1: 无界队列
     * 
     * 注意: 构造函数不会启动线程,需要显式调用start()
     */
    explicit ThreadPool(unsigned int threadCount = 0, int queueSize = 0);

    /**
     * @brief 析构函数,自动调用destroy(false)进行优雅关闭
     * 
     * 保证:
     * - 等待所有已提交的任务执行完成
     * - 吞掉所有异常(析构函数不应抛出异常)
     */
    ~ThreadPool();

    /**
     * @brief 启动线程池
     * @return 启动的线程数量
     * @throws std::runtime_error 如果启动失败
     * 
     * 流程:
     * 1. 检查是否已启动(幂等性)
     * 2. 重置停止标志
     * 3. 创建工作线程
     * 4. 如果失败,清理并重新抛出异常
     * 
     * 异常安全: 强保证(要么全部成功,要么回滚)
     */
    int start();

    /**
     * @brief 停止线程池
     * @param immediate true=立即停止(丢弃未执行任务),false=优雅关闭(等待任务完成)
     * 
     * 流程:
     * 1. 检查是否已停止
     * 2. 设置停止标志
     * 3. 如果立即停止,清空队列
     * 4. 唤醒所有等待线程
     * 5. 等待所有线程退出
     * 
     * 行为说明:
     * - immediate=false: 等待队列中的所有任务执行完成后再停止(默认)
     * - immediate=true:  立即停止,清空队列,丢弃未执行的任务
     */
    void destroy(bool immediate = false);

    /**
     * @brief 提交任务并返回future
     * @param f 可调用对象
     * @param args 参数
     * @return std::future<返回类型>
     * @throws std::runtime_error 如果线程池已停止
     * 
     * 特性:
     * - 使用条件编译的result_type自动推导返回类型
     * - 通过future获取返回值或异常
     * - 如果队列已满会阻塞,直到有空位
     * 
     * 注意: 此方法会阻塞直到队列有空位(如果队列已满)
     */
    template<typename F, typename... Args>
    auto submitTask(F&& f, Args&&... args)
        -> std::future<result_type<F, Args...>>;

    /**
     * @brief 提交任务(不返回future)
     * @param f 可调用对象
     * @param args 参数
     * @throws std::runtime_error 如果线程池已停止
     * 
     * 特性:
     * - 适合不关心返回值的场景
     * - 性能略优于submitTask(无需创建promise/future)
     * 
     * 注意: 此方法会阻塞直到队列有空位(如果队列已满)
     */
    template<typename F, typename... Args>
    void submit(F&& f, Args&&... args);

    /**
     * @brief 获取线程数量
     */
    unsigned int getThreadCount() const { return m_threadCount; }
    
    /**
     * @brief 获取队列容量(-1表示无界)
     */
    int getQueueSize() const { return m_queueSize; }
    
    /**
     * @brief 获取状态(0=运行中,-1=未启动或已停止)
     */
    int getStatus() const { return m_status.load(); }

private:
    // 任务类型定义
    using Task = std::function<void()>;

    /**
     * @brief 工作线程函数
     * 
     * 流程:
     * 1. 等待任务或停止信号
     * 2. 取出任务
     * 3. 释放锁后执行任务
     * 4. 捕获并记录异常
     * 5. 循环直到停止且队列为空
     */
    void workerThread();

    /**
     * @brief 辅助函数:设置promise值(非void返回类型)
     * 
     * 使用SFINAE在编译期选择正确的重载版本
     * 这是C++11兼容的方式,C++17可以用if constexpr简化
     */
    template<typename ReturnType, typename Func, typename Promise>
    static typename std::enable_if<!std::is_void<ReturnType>::value>::type
    setPromiseValue(Promise& promise, Func& func) {
        promise->set_value(func());
    }

    /**
     * @brief 辅助函数:设置promise值(void返回类型)
     * 
     * 特殊处理void返回类型:先调用func(),再set_value()
     */
    template<typename ReturnType, typename Func, typename Promise>
    static typename std::enable_if<std::is_void<ReturnType>::value>::type
    setPromiseValue(Promise& promise, Func& func) {
        func();
        promise->set_value();
    }

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

private:
    // ===== 配置参数 =====
    unsigned int m_threadCount;  // 线程数量
    int m_queueSize;             // 队列容量(-1=无界)

    // ===== 状态管理 =====
    std::atomic<int> m_status;   // 线程池状态(0=运行,-1=停止)
    std::atomic<bool> m_stop;    // 停止标志
    std::mutex m_lifecycleMutex; // 生命周期操作互斥锁(start/destroy)

    // ===== 任务队列 =====
    std::mutex m_mutex;                  // 任务队列互斥锁
    std::condition_variable m_cvNotEmpty; // 队列非空条件变量
    std::condition_variable m_cvNotFull;  // 队列未满条件变量
    std::queue<Task> m_tasks;            // 任务队列

    // ===== 工作线程 =====
    std::vector<std::thread> m_workers;  // 工作线程集合
};

/************************ 实现部分 ************************/

/**
 * 构造函数实现
 * 
 * 初始化策略:
 * - threadCount=0: 使用CPU核数*2作为线程数
 * - queueSize=0: 使用CPU核数*10作为队列容量
 * - queueSize=-1: 无界队列
 * 
 * 注意:
 * - 构造函数不启动线程,需要显式调用start()
 * - 状态初始化为-1(未启动)
 */
inline ThreadPool::ThreadPool(unsigned int threadCount, int queueSize)
    : m_status(-1), m_stop(false)
{
    // 获取硬件并发数
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 2; // 降级默认值
    
    // 设置线程数
    m_threadCount = (threadCount == 0) ? cores * 2 : threadCount;
    
    // 设置队列容量
    if (queueSize == 0) {
        m_queueSize = static_cast<int>(cores * 10);
    } else {
        m_queueSize = queueSize;
    }
}

/**
 * 析构函数实现
 * 
 * 保证:
 * - 优雅关闭(等待任务完成)
 * - 吞掉所有异常(析构函数不应抛出异常)
 */
inline ThreadPool::~ThreadPool() {
    try {
        destroy(false);
    } catch (...) {
        // 析构函数中吞掉异常
    }
}

/**
 * 启动线程池
 * 
 * 流程:
 * 1. 加锁保护(防止并发start)
 * 2. 检查是否已启动(幂等性)
 * 3. 重置停止标志和工作线程容器
 * 4. 创建工作线程
 * 5. 如果失败,清理资源并重新抛出异常
 * 
 * 异常安全: 强保证(要么全部成功,要么完全回滚)
 */
inline int ThreadPool::start() {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    
    // 已启动,直接返回
    if (m_status.load() == 0) {
        return static_cast<int>(m_threadCount);
    }

    // 重置状态
    m_stop.store(false);
    m_workers.clear();
    m_workers.reserve(m_threadCount);

    try {
        // 创建工作线程
        for (unsigned int i = 0; i < m_threadCount; ++i) {
            m_workers.emplace_back(&ThreadPool::workerThread, this);
        }
        m_status.store(0);
        return static_cast<int>(m_threadCount);
    } catch (...) {
        // 启动失败,清理资源
        m_stop.store(true);
        m_cvNotEmpty.notify_all();
        for (auto& w : m_workers) {
            if (w.joinable()) w.join();
        }
        m_workers.clear();
        throw;
    }
}

/**
 * 停止线程池
 * 
 * 流程:
 * 1. 检查是否已停止
 * 2. 设置停止标志
 * 3. 如果立即停止,清空队列
 * 4. 唤醒所有等待线程
 * 5. 等待所有线程退出
 * 
 * @param immediate 
 *   - true: 立即停止,丢弃未执行的任务
 *   - false: 优雅关闭,等待所有任务执行完成
 */
inline void ThreadPool::destroy(bool immediate) {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    
    // 未启动或已停止
    if (m_status.load() != 0) {
        return;
    }

    // 设置停止标志
    m_stop.store(true);

    // 立即停止:清空任务队列
    if (immediate) {
        std::lock_guard<std::mutex> qlock(m_mutex);
        while (!m_tasks.empty()) {
            m_tasks.pop();
        }
    }

    // 唤醒所有等待的线程
    // 消费者(工作线程)会检查m_stop并退出
    // 生产者(提交任务线程)会检查m_stop并抛出异常
    m_cvNotEmpty.notify_all();
    m_cvNotFull.notify_all();

    // 等待所有工作线程退出
    for (auto& w : m_workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    m_workers.clear();
    m_status.store(-1);
}

/**
 * 工作线程主循环
 * 
 * 核心逻辑:
 * 1. 持锁等待:任务可用 或 停止信号
 * 2. 如果停止且队列空,退出循环
 * 3. 取出任务,通知生产者(队列有空位)
 * 4. 释放锁
 * 5. 执行任务(捕获异常)
 * 6. 重复
 * 
 * 关键点:
 * - 通知在持锁时进行,确保生产者立即被唤醒
 * - 任务执行在锁外进行,避免阻塞其他线程
 * - 异常被捕获并记录,不会导致线程退出
 */
inline void ThreadPool::workerThread() {
    while (true) {
        Task task;
        
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            
            // 等待任务或停止信号
            // spurious wakeup是安全的,会重新检查条件
            m_cvNotEmpty.wait(lock, [this] {
                return m_stop.load() || !m_tasks.empty();
            });

            // 停止信号且队列为空,退出
            if (m_stop.load() && m_tasks.empty()) {
                return;
            }

            // 取出任务
            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop();
                
                // 关键:通知必须在持锁时进行
                // 这确保等待的生产者能立即被唤醒并入队
                m_cvNotFull.notify_one();
            }
        } // 释放锁

        // 执行任务(在锁外执行,避免阻塞)
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[ThreadPool] Exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[ThreadPool] Unknown exception" << std::endl;
            }
        }
    }
}

/**
 * 提交任务并返回future
 * 
 * 流程:
 * 1. 使用条件编译的result_type推导返回类型
 * 2. 创建promise和future
 * 3. 绑定参数(使用std::bind延迟求值)
 * 4. 包装为Task(捕获异常并设置promise)
 * 5. 等待队列有空位
 * 6. 入队并通知消费者
 * 7. 返回future
 * 
 * C++11/17兼容性:
 * - 使用条件编译的result_type,自动选择std::result_of或std::invoke_result
 * - 使用SFINAE替代C++17的if constexpr
 * - 通过辅助函数setPromiseValue处理void/非void返回类型
 * 
 * 异常安全:
 * - 如果线程池停止,通过promise传递异常给调用者
 * - 如果任务执行失败,通过promise传递异常给调用者
 */
template<typename F, typename... Args>
inline auto ThreadPool::submitTask(F&& f, Args&&... args)
    -> std::future<result_type<F, Args...>>
{
    // 推导返回类型(使用条件编译的result_type)
    using ReturnType = result_type<F, Args...>;
    
    // 创建promise和future
    auto promise = std::make_shared<std::promise<ReturnType>>();
    auto future = promise->get_future();
    
    // 绑定参数(延迟求值)
    auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    
    // 包装任务
    // 注意:必须用mutable,因为func可能会修改自身状态
    Task task = [promise, func]() mutable {
        try {
            // 使用SFINAE处理void/非void返回类型
            setPromiseValue<ReturnType>(promise, func);
        } catch (...) {
            // 捕获任务执行异常,通过promise传递
            promise->set_exception(std::current_exception());
        }
    };

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        // 等待队列有空位
        // 条件:(停止) 或 (无界队列) 或 (队列未满)
        m_cvNotFull.wait(lock, [this] {
            return m_stop.load() || 
                   m_queueSize == -1 || 
                   static_cast<int>(m_tasks.size()) < m_queueSize;
        });

        // 如果线程池已停止,设置异常并抛出
        if (m_stop.load()) {
            promise->set_exception(
                std::make_exception_ptr(std::runtime_error("ThreadPool stopped"))
            );
            throw std::runtime_error("ThreadPool stopped");
        }

        // 入队
        m_tasks.emplace(std::move(task));
        
        // 关键:通知必须在持锁时进行
        m_cvNotEmpty.notify_one();
    }

    return future;
}

/**
 * 提交任务(不返回future)
 * 
 * 与submitTask类似,但不创建promise/future
 * 适合不关心返回值的场景,性能略好
 * 
 * 异常处理:
 * - 任务执行异常被捕获并记录(不传递给调用者)
 * - 如果线程池停止,抛出异常
 */
template<typename F, typename... Args>
inline void ThreadPool::submit(F&& f, Args&&... args) {
    // 绑定参数
    auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    
    // 包装任务
    Task task = [func]() mutable {
        try {
            func();
        } catch (const std::exception& e) {
            std::cerr << "[ThreadPool] Exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[ThreadPool] Unknown exception" << std::endl;
        }
    };

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        // 等待队列有空位
        m_cvNotFull.wait(lock, [this] {
            return m_stop.load() || 
                   m_queueSize == -1 || 
                   static_cast<int>(m_tasks.size()) < m_queueSize;
        });

        // 线程池已停止,抛出异常
        if (m_stop.load()) {
            throw std::runtime_error("ThreadPool stopped");
        }

        // 入队
        m_tasks.emplace(std::move(task));
        
        // 关键:通知必须在持锁时进行
        m_cvNotEmpty.notify_one();
    }
}

} // namespace xdg

#endif // _XDG_THREAD_POOL_HPP_

