#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <numeric>
#include "xdgThreadPool.hpp"

// 用于在多线程环境下安全打印到控制台的互斥锁
std::mutex g_cout_mutex;

/**
 * @brief 线程安全的打印函数 (C++11 实现)
 */
template<typename... Args>
void print_safe(Args&&... args) {
    std::lock_guard<std::mutex> lock(g_cout_mutex);
    using Expander = int[];
    Expander{ (std::cout << std::forward<Args>(args), 0)... };
    std::cout << std::endl;
}

/**
 * @brief 打印一个美化的测试标题
 */
void print_header(const std::string& title) {
    print_safe("\n=================================================");
    print_safe("  ", title);
    print_safe("=================================================");
}

// 全局原子计数器，用于在并发环境中安全地计数
std::atomic<long long> g_task_counter{0};


// --- 功能验证测试 ---

void test_basic_and_future() {
    print_header("功能测试 1: 基本提交与 Future");
    xdg::ThreadPool pool(2, 4);
    pool.start();
    print_safe("线程池已启动。");
    g_task_counter = 0;

    pool.submit([]{
        print_safe("  > (任务A) fire-and-forget 任务执行...");
        g_task_counter++;
    });

    auto future = pool.submitTask([](int a, int b) {
        print_safe("  > (任务B) 带返回值的任务执行...");
        return a + b;
    }, 100, 200);

    print_safe("主线程: 等待任务B的结果...");
    int result = future.get();
    print_safe("主线程: 任务B结果为: ", result, " (预期: 300)");

    if (result == 300) print_safe("[OK] Future 返回值正确。");
    else print_safe("[FAIL] Future 返回值错误。");

    // 等待任务A执行
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_task_counter.load() == 1) print_safe("[OK] fire-and-forget 任务已执行。");
    else print_safe("[FAIL] fire-and-forget 任务未执行。");
}

void test_shutdown_modes() {
    print_header("功能测试 2: 两种关闭模式 (优雅 vs 立即)");
    
    print_safe("\n--- 子测试: 优雅关闭 (destroy(false)) ---");
    {
        xdg::ThreadPool pool(1);
        pool.start();
        g_task_counter = 0;
        
        pool.submit([]{ std::this_thread::sleep_for(std::chrono::milliseconds(100)); g_task_counter++; });
        pool.submit([]{ g_task_counter++; });
        pool.submit([]{ g_task_counter++; });
        pool.submit([]{ g_task_counter++; });

        print_safe("调用 destroy(false)，等待所有4个任务完成...");
    } // pool 在此离开作用域，调用析构函数 -> destroy(false)
    
    if (g_task_counter.load() == 4) print_safe("[OK] 优雅关闭：所有4个任务都已执行。");
    else print_safe("[FAIL] 优雅关闭：任务执行不完整，计数: ", g_task_counter.load());

    print_safe("\n--- 子测试: 立即关闭 (destroy(true)) ---");
    {
        xdg::ThreadPool pool(1);
        pool.start();
        g_task_counter = 0;

        pool.submit([]{
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            g_task_counter++;
        });
        
        auto future_canceled = pool.submitTask([]{ g_task_counter++; return 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        print_safe("调用 destroy(true)，队列中未执行的任务将被取消...");
        pool.destroy(true); 
        print_safe("立即关闭完成。");

        if (g_task_counter.load() <= 1) print_safe("[OK] 立即关闭：只有正在执行的任务完成了。");
        else print_safe("[FAIL] 立即关闭：队列中的任务被错误地执行了。");

        // 验证被取消任务的future状态
        try {
            future_canceled.get();
            print_safe("[FAIL] 被取消任务的future没有按预期抛出异常。");
        } catch (const std::runtime_error& e) {
            print_safe("[OK] 成功捕获到被取消任务的异常: ", e.what());
        } catch (...) {
            print_safe("[FAIL] 捕获到未知异常。");
        }
    }
}

void test_exception_handling() {
    print_header("功能测试 3: 异常安全");
    xdg::ThreadPool pool(2);
    pool.start();

    print_safe("提交一个会抛异常的 fire-and-forget 任务...");
    pool.submit([]{ throw std::logic_error("fire-and-forget 异常"); });

    print_safe("提交一个会抛异常的 future 任务...");
    auto future = pool.submitTask([]{ throw std::runtime_error("future 异常"); });
    
    try {
        print_safe("主线程调用 future.get()，预期会捕获到异常...");
        future.get();
    } catch (const std::runtime_error& e) {
        print_safe("[OK] 成功从 future 捕获到异常: ", e.what());
    } catch (...) {
        print_safe("[FAIL] 从 future 捕获到未知异常。");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void test_resettable() {
    print_header("功能测试 4: 重复启停");
    xdg::ThreadPool pool(1);

    print_safe("--- 第一次运行 ---");
    pool.start();
    auto fut1 = pool.submitTask([]{ return 1; });
    if (fut1.get() == 1) print_safe("[OK] 第一次运行结果正确。");
    else print_safe("[FAIL] 第一次运行结果错误。");
    pool.destroy();
    print_safe("第一次销毁完成。");

    print_safe("\n--- 第二次运行 ---");
    pool.start();
    auto fut2 = pool.submitTask([]{ return 2; });
    if (fut2.get() == 2) print_safe("[OK] 第二次运行结果正确。");
    else print_safe("[FAIL] 第二次运行结果错误。");
    pool.destroy();
    print_safe("第二次销毁完成。");
}


// --- 性能测试 ---

/**
 * @brief 测试用的任务函数，模拟少量CPU工作负载
 */
void performance_task() {
    // volatile关键字防止编译器优化掉这个循环
    volatile int x = 0;
    for (int i = 0; i < 100; ++i) {
        x += i;
    }
    g_task_counter++;
}

void test_performance_and_scalability() {
    print_header("性能测试: 吞吐量与扩展性");

    const int NUM_TASKS = 2000000;
    const unsigned int core_count = std::thread::hardware_concurrency();
    print_safe("任务总量: ", NUM_TASKS, ", CPU核心数: ", core_count);

    // 1. 单线程基准
    print_safe("\n--- 1. 单线程基准测试 (串行执行) ---");
    g_task_counter = 0;
    auto start_time = std::chrono::high_resolution_clock::now();
    for(int i = 0; i < NUM_TASKS; ++i) { performance_task(); }
    auto end_time = std::chrono::high_resolution_clock::now();
    long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    if (duration_ms == 0) duration_ms = 1;
    double throughput = (double)NUM_TASKS / duration_ms * 1000.0;
    print_safe("  > 总耗时:   ", duration_ms, " ms, 吞吐量: ", (long long)throughput, " tasks/sec");
    print_safe("  > 任务验证: ", g_task_counter.load() == NUM_TASKS ? "[OK]" : "[FAIL]");

    // 2. 线程池扩展性测试
    print_safe("\n--- 2. 线程池扩展性测试 (并行执行) ---");
    std::vector<unsigned int> thread_counts = {1, 2, 4};
    if (core_count > 4) thread_counts.push_back(core_count);
    if (core_count > 8 && core_count * 2 > core_count) thread_counts.push_back(core_count * 2);

    for (unsigned int n : thread_counts) {
        print_safe("\n[正在测试 ", n, " 个工作线程...]");
        xdg::ThreadPool pool(n);
        pool.start();
        g_task_counter = 0;

        start_time = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_TASKS; ++i) {
            pool.submit(performance_task);
        }
        pool.destroy(false);
        end_time = std::chrono::high_resolution_clock::now();

        duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        if (duration_ms == 0) duration_ms = 1;
        throughput = (double)NUM_TASKS / duration_ms * 1000.0;
        
        print_safe("  > 总耗时:   ", duration_ms, " ms, 吞吐量: ", (long long)throughput, " tasks/sec");
        print_safe("  > 任务验证: ", g_task_counter.load() == NUM_TASKS ? "[OK]" : "[FAIL]");
    }
}


int main(int argc, char** argv) {
    // --- 功能验证测试 ---
    test_basic_and_future();
    test_shutdown_modes();
    test_exception_handling();
    test_resettable();

    // --- 性能与扩展性测试 ---
    test_performance_and_scalability();

    print_safe("\n所有测试已完成。");

    return 0;
}
