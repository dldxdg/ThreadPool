#ifndef _THREAD_POOL_HPP_
#define _THREAD_POOL_HPP_

#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <type_traits> // for std::decay
#include <utility>     // for std::forward
#include <memory>      // for std::make_shared
#include <stdexcept>   // for std::runtime_error

namespace xdg {

class ThreadPool {
/************************构造/析构函数************************/
public:
    /*
    * 构造函数
    * @param ThreadCount 线程数量，0 为默认（cpu核心数 * 2）
    * @param QueueSize 任务队列大小，-1 为默认（不限制），0 为（cpu核数 * 10）
    */
    ThreadPool(unsigned int ThreadCount = 0, int QueueSize = -1);

    /*
    * 析构函数
    */
    ~ThreadPool();

/************************共有成员函数************************/
public:
    /*
    * 开启线程池
    * @return 返回创建成功的线程数量
    */
    int start();

    /*
    * 销毁线程池
    */
    void destroy();

    /*
    * 获取工作线程数量
    * @return 工作线程数量
    */
    unsigned int getThreadCount() const;

    /*
    * 获取任务队列容量
    * @return 任务队列容量
    */
    int getQueueSize() const;

    /*
    * 获取当前线程池状态
    * @return 线程池状态。-1 为未创建/销毁，0 为运行中
    */
    int getStatus() const;

    /*
    * 向线程池提交异步任务,
    * @param f 任务函数
    * @param args 任务函数的参数
    * @return 返回 std::future，用于获取任务执行结果
    */
    template <typename F, typename... Args>
    auto submitTask(F&& f, Args&&... args) ->
        std::future<typename std::decay<decltype(std::forward<F>(f)(std::forward<Args>(args)...))>::type>;

    /*
    * 向线程池提交异步任务 (即发即忘)
    * @param f 任务函数
    * @param args 任务函数的参数
    */
    template <typename F, typename... Args>
    void submit(F&& f, Args&&... args);

/************************私有成员变量和函数************************/
private:
    /**
    * 工作线程函数，每个线程启动后都会循环执行此函数
    */
    void worker_loop();

    // ------------------ 核心成员变量 ------------------

    // --- 配置参数 ---
    unsigned int m_ThreadCount; // 线程数量
    int m_QueueSize;          // 任务队列容量

    // --- 状态与生命周期控制 ---
    std::atomic<int> m_Status;           // 线程池状态
    std::atomic<bool> m_stopFlag;        // 线程池停止标志
    std::mutex m_lifecycleMutex;         // 用于保护 start/destroy 的互斥锁
    std::once_flag m_startFlag;          // 用于保证 start 逻辑只被调用一次

    // --- 任务队列与同步机制 (单锁模型) ---
    std::mutex m_queueMutex;             // 【修改】唯一的、保护任务队列的互斥锁
    std::condition_variable m_notFull;   // 条件变量：当队列不满时通知生产者
    std::condition_variable m_notEmpty;  // 条件变量：当队列不空时通知消费者
    std::deque<std::function<void()>> m_TaskQueue; // 存储任务的队列

    // --- 线程管理 ---
    std::vector<std::thread> m_Threads;  // 存储所有工作线程的容器
};



// ----------------------------------------------------------------------------
// --- 方法实现 ---
// ----------------------------------------------------------------------------

// 构造函数：初始化配置参数
inline ThreadPool::ThreadPool(unsigned int ThreadCount, int QueueSize)
    : m_Status(-1), m_stopFlag(false)
{
    // --- 计算并设置最终的线程数量 ---
    unsigned int core_count = std::thread::hardware_concurrency();
    if (ThreadCount == 0) {
        m_ThreadCount = core_count > 0 ? core_count * 2 : 4; // 如果无法获取核心数，默认4个
    } else {
        m_ThreadCount = ThreadCount;
    }

    // --- 计算并设置最终的队列容量 ---
    if (QueueSize == 0) {
        m_QueueSize = core_count > 0 ? core_count * 10 : 20; // 如果无法获取核心数，默认20
    } else {
        m_QueueSize = QueueSize; // 接受用户指定的正数值，或者 -1 代表无界
    }
}

// 析构函数：确保线程池在对象销毁时能被正确关闭
inline ThreadPool::~ThreadPool()
{
    destroy();
}

// 开启线程池 (支持重复启动)
inline int ThreadPool::start()
{
    // 使用互斥锁保护，防止多线程同时调用 start
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);

    // 只有在“已销毁”状态下才能启动
    if (m_Status != -1) {
        return m_ThreadCount; // 如果已启动，直接返回
    }

    // --- 重置状态以支持重启 ---
    m_stopFlag.store(false);

    // 创建并启动工作线程
    m_Threads.reserve(m_ThreadCount);
    for (unsigned int i = 0; i < m_ThreadCount; ++i) {
        m_Threads.emplace_back(&ThreadPool::worker_loop, this);
    }
    m_Status = 0; // 设置状态为运行中

    return m_ThreadCount;
}

// 销毁线程池 (为重启做准备)
inline void ThreadPool::destroy()
{
    // 使用互斥锁保护，防止多线程同时调用
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);

    if (m_Status == -1) {
        return; // 如果已销毁，直接返回
    }

    // 步骤1：设置停止标志并清空任务队列
    {
        // 锁住任务队列，确保在清空和设置标志时没有其他线程在操作队列
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_stopFlag.store(true);
        // 清空所有待处理的任务，为下一次 start 做准备
        m_TaskQueue.clear(); 
    }

    // 步骤2：唤醒所有可能在等待的线程
    m_notFull.notify_all();
    m_notEmpty.notify_all();

    // 步骤3：等待所有工作线程执行完毕
    for (auto& t : m_Threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    // 步骤4：清理线程容器并重置状态
    m_Threads.clear();
    m_Status = -1; // 设置为“已销毁”状态，允许下一次调用 start
}

// 工作线程函数 (消费者)
inline void ThreadPool::worker_loop()
{
    while (!m_stopFlag.load()) {
        std::function<void()> task; // 任务函数

        {
            // 锁住队列互斥锁，准备安全地访问任务队列
            std::unique_lock<std::mutex> lock(m_queueMutex); // 使用唯一的队列锁

            // 等待直到“队列不为空”或“线程池停止”
            m_notEmpty.wait(lock, [this] {
                return m_stopFlag.load() || !m_TaskQueue.empty();
            });

            // 被唤醒后，优先检查是否是因停止信号而醒来
            if (m_stopFlag.load() && m_TaskQueue.empty()) {
                return; // 退出循环，线程结束
            }

            // 从队列头部取出一个任务
            task = std::move(m_TaskQueue.front());
            m_TaskQueue.pop_front();

            // 通知一个可能正在等待的生产者：队列有空位了
            m_notFull.notify_one();

        } // 锁释放

        // 在锁之外执行任务，避免任务执行时间过长而阻塞其他线程
        try {
            if (task) task();
        } catch (const std::exception& e) {
            fprintf(stderr, "Task threw a standard exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "Task threw a non-standard or unknown exception.\n");
        }

    } // while (!m_stopFlag.load()) 结束
}

// 提交任务，带返回值 (生产者)
template <typename F, typename... Args>
inline auto ThreadPool::submitTask(F&& f, Args&&... args) ->
    std::future<typename std::decay<decltype(std::forward<F>(f)(std::forward<Args>(args)...))>::type>
{
    using return_type = typename std::decay<decltype(std::forward<F>(f)(std::forward<Args>(args)...))>::type;

    std::unique_lock<std::mutex> lock(m_queueMutex); // 使用唯一的队列锁

    // 等待直到“队列不满”或“线程池停止”
    m_notFull.wait(lock, [this] {
        return m_stopFlag.load() || (m_QueueSize == -1 || m_TaskQueue.size() < m_QueueSize);
    });

    if (m_stopFlag.load()) {
        throw std::runtime_error("submitTask on a stopped ThreadPool");
    }

    auto bound_task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(bound_task);
    std::future<return_type> future = task_ptr->get_future();
    m_TaskQueue.emplace_back([task_ptr]() {
        (*task_ptr)();
    });

    // 通知一个可能正在等待的消费者：有新任务了
    m_notEmpty.notify_one();

    return future;
}

// 提交任务，无返回值 (生产者)
template <typename F, typename... Args>
inline void ThreadPool::submit(F&& f, Args&&... args)
{
    std::unique_lock<std::mutex> lock(m_queueMutex); // 使用唯一的队列锁

    m_notFull.wait(lock, [this] {
        return m_stopFlag.load() || (m_QueueSize == -1 || m_TaskQueue.size() < m_QueueSize);
    });

    if (m_stopFlag.load()) {
        throw std::runtime_error("submit on a stopped ThreadPool");
    }

    auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    m_TaskQueue.emplace_back(std::move(task));

    m_notEmpty.notify_one();
}

// --- Getter 函数 ---
inline unsigned int ThreadPool::getThreadCount() const { return m_ThreadCount; }
inline int ThreadPool::getQueueSize() const { return m_QueueSize; }
inline int ThreadPool::getStatus() const { return m_Status.load(); }

} // namespace xdg

#endif // _THREAD_POOL_HPP_