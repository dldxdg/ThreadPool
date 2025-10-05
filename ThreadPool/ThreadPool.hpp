#ifndef _THREAD_POOL_HPP_
#define _THREAD_POOL_HPP_

#include <iostream>
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
    * @param immediate 如果为 true，则立刻清空任务队列并销毁；
    * 如果为 false，将拒绝新的提交并等待队列完成，并等待所有已提交的任务执行完毕后再销毁。
    */
    void destroy(bool immediate = false); // 默认行为是立刻销毁

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
    auto submitTask(F&& f, Args&&... args)
        -> std::future<typename std::decay<typename std::result_of<F(Args...)>::type>::type>;

    /*
    * 向线程池提交异步任务 (即发即忘)
    * @param f 任务函数
    * @param args 任务函数的参数
    */
    template <typename F, typename... Args>
    void submit(F&& f, Args&&... args);

/************************私有成员变量和函数************************/
private:
    ThreadPool(const ThreadPool&) = delete; // 禁止拷贝构造
    ThreadPool& operator=(const ThreadPool&) = delete; // 禁止赋值运算符
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

// 析构函数：默认执行“优雅关闭”，避免数据丢失
inline ThreadPool::~ThreadPool()
{
    // 在析构中做“优雅关闭”的默认行为（等待队列任务执行完） 注意：析构函数内不能抛异常
    try {
        destroy(false); // 调用优雅关闭，等待所有任务完成。这是最安全的默认行为。
    } catch (...) {
        // 保证析构不抛异常；可以用日志记录
    }
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
    m_Threads.clear();
    m_Threads.reserve(m_ThreadCount);

    try {
        for (unsigned int i = 0; i < m_ThreadCount; ++i) {
            m_Threads.emplace_back(&ThreadPool::worker_loop, this);
        }
        m_Status = 0;
    } catch (...) {
        // 创建线程过程中抛出：清理已创建线程
        m_stopFlag.store(true);
        for (auto &t : m_Threads) {
            if (t.joinable()) t.join();
        }
        m_Threads.clear();
        m_Status = -1;
        throw;
    }

    return m_ThreadCount;
}


// 销毁线程池 (支持两种模式)
inline void ThreadPool::destroy(bool immediate)
{
    // 使用互斥锁保护，确保 destroy 函数是线程安全的
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);

    if (m_Status == -1) {
        return; // 如果已销毁，直接返回
    }

    // 步骤1：设置停止标志，并根据模式决定是否清空任务队列
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        
        // 通知生产者不能再提交任务
        m_stopFlag.store(true);

        if (immediate) {
            // 立刻销毁模式：清空所有待处理的任务
            m_TaskQueue.clear(); 
        }
        // 优雅关闭模式：不清空队列，让工作线程继续执行
    }

    // 步骤2：唤醒所有可能在等待的线程
    // 无论是生产者还是消费者，都需要被唤醒，以便它们能观察到 m_stopFlag 的变化
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
    while (1) {
        std::function<void()> task; // 任务函数

        {
            // 锁住队列互斥锁，准备安全地访问任务队列
            std::unique_lock<std::mutex> lock(m_queueMutex); // 使用唯一的队列锁

            // 等待直到“队列不为空”或“线程池停止”
            m_notEmpty.wait(lock, [this] {
                return m_stopFlag.load() || !m_TaskQueue.empty();
            });

            // 【关键的退出逻辑】
            // 这是整个循环唯一的、正确的退出点。
            // 它确保了即使 m_stopFlag 为 true，只要队列中还有任务，循环就不会退出。
            if (m_stopFlag.load() && m_TaskQueue.empty()) {
                return; // 退出循环，线程结束
            }

            // 如果能走到这里，说明队列必定不为空，从队列头部取出一个任务
            task = std::move(m_TaskQueue.front());
            m_TaskQueue.pop_front();

        } // 锁释放

        // 通知一个可能正在等待的生产者：队列有空位了
        m_notFull.notify_one();

        // 在锁之外执行任务，避免任务执行时间过长而阻塞其他线程
        try {
            if (task) task();
        } catch (const std::exception& e) {
            std::cerr << "Task threw a standard exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Task threw an unknown exception.\n";
        }

    } // while (!m_stopFlag.load()) 结束
}

// 提交任务，带返回值 (生产者)
template <typename F, typename... Args>
inline auto ThreadPool::submitTask(F&& f, Args&&... args)
    -> std::future<typename std::decay<typename std::result_of<F(Args...)>::type>::type>
{
    using result_t = typename std::decay<typename std::result_of<F(Args...)>::type>::type;

    // promise + future（使用 shared_ptr 以便在 lambda/catch 路径中共享）
    auto prom = std::make_shared<std::promise<result_t>>();
    std::future<result_t> fut = prom->get_future();

    // C++11 下使用 std::bind 将可调用对象与参数绑定
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

    // 构造任务：执行 bound 并把结果/异常写入 promise
    auto task = [prom, bound]() mutable {
        try {
            if (std::is_void<result_t>::value) {
                bound();               // 执行
                prom->set_value();     // void 特殊处理
            } else {
                prom->set_value(bound());
            }
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
    };


    // 将任务放入队列：在任何抛出路径上设置 promise 的异常，避免 future 永远等待
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_notFull.wait(lock, [this] {
            return m_stopFlag.load() || (m_QueueSize == -1 || (int)m_TaskQueue.size() < m_QueueSize);
        });

        if (m_stopFlag.load()) {
            // 把异常传给 future，再抛异常给调用者
            prom->set_exception(std::make_exception_ptr(std::runtime_error("submitTask on a stopped ThreadPool")));
            throw std::runtime_error("submitTask on a stopped ThreadPool");
        }

        try {
            m_TaskQueue.emplace_back(std::move(task));
        } catch (...) {
            // emplace_back（如 bad_alloc）抛出时，必须把异常传给 promise，避免 future 永远等待
            prom->set_exception(std::current_exception());
            throw;
        }

        
    } // 锁释放
    // 唤醒一个可能正在等待的消费者
    m_notEmpty.notify_one();
    return fut;
}

// 提交任务，无返回值 (生产者)
template <typename F, typename... Args>
inline void ThreadPool::submit(F&& f, Args&&... args)
{
    // 在函数体内声明类型别名（C++11 支持）
    using Task = std::function<void()>;

    // 先把可调用对象与参数绑定（可能抛出，但在锁外更好）
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

    // 用 Task 封装，任务内部捕获异常防止线程终止
    Task task = [bound]() mutable {
        try {
            bound();
        } catch (const std::exception& e) {
            // 可选：记录或上报错误，避免异常传播导致线程退出
            std::cerr << "fire-and-forget task threw a standard exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "fire-and-forget task threw an unknown exception.\n";
        }
    };

    // 将任务推入队列（在锁内操作队列结构）
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_notFull.wait(lock, [this] {
        return m_stopFlag.load() || (m_QueueSize == -1 || (int)m_TaskQueue.size() < m_QueueSize);
    });

    if (m_stopFlag.load()) {
        throw std::runtime_error("submit on a stopped ThreadPool");
    }

    try {
        m_TaskQueue.emplace_back(std::move(task));
    } catch (...) {
        // emplace_back 可能抛（例如 bad_alloc），这里把异常向上抛给调用方
        // 如果你想在 emplace_back 失败时改为在当前线程执行任务，可以在这里调用 `bound()`（谨慎）
        throw;
    }

    // 解锁后通知消费者，减少唤醒后竞争锁的概率
    lock.unlock();
    m_notEmpty.notify_one();
}

// --- Getter 函数 ---
inline unsigned int ThreadPool::getThreadCount() const { return m_ThreadCount; }
inline int ThreadPool::getQueueSize() const { return m_QueueSize; }
inline int ThreadPool::getStatus() const { return m_Status.load(); }

} // namespace xdg

#endif // _THREAD_POOL_HPP_