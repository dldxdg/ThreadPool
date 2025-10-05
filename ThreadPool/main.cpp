#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>

// 包含你编写的线程池头文件
#include "ThreadPool.hpp"

// 用于测试的全局原子计数器
std::atomic<int> g_task_counter{0};
// 用于输出同步的互斥锁
std::mutex g_cout_mutex;

// Helper to print thread-safe messages
template<typename... Args>
void print_safe(Args&&... args) {
    std::lock_guard<std::mutex> lock(g_cout_mutex);
    (std::cout << ... << args) << std::endl;
}

// 打印测试标题的辅助函数
void print_header(const std::string& title) {
    print_safe("\n=================================================");
    print_safe("  ", title);
    print_safe("=================================================");
}

// --- 测试用例 ---

void test_basic_and_future() {
    print_header("测试 1: 基本提交与 Future");
    xdg::ThreadPool pool(2); // 创建2个线程的线程池
    pool.start();
    print_safe("线程池已启动。");

    g_task_counter = 0;

    // 1. 测试 fire-and-forget (即发即忘)
    pool.submit([]{
        print_safe("  > (任务 A) fire-and-forget 任务正在执行...");
        g_task_counter++;
    });

    // 2. 测试带返回值的任务
    auto future = pool.submitTask([](int a, int b) {
        print_safe("  > (任务 B) 带返回值的任务正在执行...");
        return a + b;
    }, 10, 20);

    print_safe("主线程：等待任务 B 的结果...");
    int result = future.get(); // get() 会阻塞直到结果返回
    print_safe("主线程：任务 B 的结果是: ", result, " (预期: 30)");

    if (result == 30) {
        print_safe("[OK] Future 返回值正确。");
    } else {
        print_safe("[FAIL] Future 返回值错误。");
    }

    // 等待任务 A 执行完毕
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_task_counter.load() == 1) {
        print_safe("[OK] fire-and-forget 任务已执行。");
    } else {
        print_safe("[FAIL] fire-and-forget 任务未执行。");
    }

    pool.destroy(false); // 优雅关闭
    print_safe("线程池已销毁。");
}

void test_shutdown_modes() {
    print_header("测试 2: 两种关闭模式 (优雅 vs 立即)");
    
    // --- 优雅关闭测试 ---
    print_safe("\n--- 子测试: 优雅关闭 (destroy(false)) ---");
    xdg::ThreadPool pool_graceful(1);
    pool_graceful.start();
    g_task_counter = 0;
    
    print_safe("提交一个耗时100ms的任务...");
    pool_graceful.submit([]{
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_task_counter++;
    });
    print_safe("提交3个计数任务...");
    pool_graceful.submit([]{ g_task_counter++; });
    pool_graceful.submit([]{ g_task_counter++; });
    pool_graceful.submit([]{ g_task_counter++; });

    print_safe("调用 destroy(false)，主线程将等待所有4个任务完成...");
    pool_graceful.destroy(false); // 优雅关闭
    print_safe("优雅关闭完成。");
    
    if (g_task_counter.load() == 4) {
        print_safe("[OK] 优雅关闭：所有4个任务都已执行。");
    } else {
        print_safe("[FAIL] 优雅关闭：任务执行不完整，计数: ", g_task_counter.load());
    }

    // --- 立即关闭测试 ---
    print_safe("\n--- 子测试: 立即关闭 (destroy(true)) ---");
    xdg::ThreadPool pool_immediate(1);
    pool_immediate.start();
    g_task_counter = 0;

    print_safe("提交一个耗时500ms的任务...");
    pool_immediate.submit([]{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        g_task_counter++; // 这个任务应该会开始执行
    });
     // 快速提交另外3个任务到队列
    pool_immediate.submit([]{ g_task_counter++; });
    pool_immediate.submit([]{ g_task_counter++; });
    pool_immediate.submit([]{ g_task_counter++; });

    // 在队列中的任务被执行前，立刻销毁
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    print_safe("调用 destroy(true)，队列中未执行的任务将被丢弃...");
    pool_immediate.destroy(true); // 立即关闭
    print_safe("立即关闭完成。");

    if (g_task_counter.load() <= 1) {
        print_safe("[OK] 立即关闭：只有正在执行的任务完成了，计数: ", g_task_counter.load());
    } else {
        print_safe("[FAIL] 立即关闭：队列中的任务被错误地执行了，计数: ", g_task_counter.load());
    }
}

void test_exception_handling() {
    print_header("测试 3: 异常安全");
    xdg::ThreadPool pool(2);
    pool.start();

    // 1. 测试 fire-and-forget 任务的异常
    print_safe("提交一个会抛异常的 fire-and-forget 任务 (应在 stderr 看到错误信息)...");
    pool.submit([]{
        throw std::logic_error("fire-and-forget 异常测试");
    });

    // 2. 测试带 future 任务的异常
    print_safe("提交一个会抛异常的 future 任务...");
    auto future = pool.submitTask([]{
        throw std::runtime_error("future 异常传播测试");
    });
    
    try {
        print_safe("主线程调用 future.get()，预期会捕获到异常...");
        future.get();
    } catch (const std::runtime_error& e) {
        print_safe("[OK] 成功从 future 捕获到异常: ", e.what());
    } catch (...) {
        print_safe("[FAIL] 从 future 捕获到未知异常。");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.destroy(false);
    print_safe("线程池已销毁。");
}

void test_resettable() {
    print_header("测试 4: 重复启停");
    xdg::ThreadPool pool(1);

    print_safe("--- 第一次运行 ---");
    pool.start();
    auto fut1 = pool.submitTask([]{ return 1; });
    if (fut1.get() == 1) print_safe("[OK] 第一次运行结果正确。");
    else print_safe("[FAIL] 第一次运行结果错误。");
    pool.destroy(false);
    print_safe("第一次销毁完成。");

    print_safe("\n--- 第二次运行 ---");
    pool.start();
    auto fut2 = pool.submitTask([]{ return 2; });
    if (fut2.get() == 2) print_safe("[OK] 第二次运行结果正确。");
    else print_safe("[FAIL] 第二次运行结果错误。");
    pool.destroy(false);
    print_safe("第二次销毁完成。");
}

void test_stress() {
    print_header("测试 5: 并发压力测试");
    const int num_producers = 4;
    const int tasks_per_producer = 500;
    xdg::ThreadPool pool(4); // 4个工作线程
    pool.start();
    g_task_counter = 0;

    print_safe("启动 ", num_producers, " 个生产者线程，每个提交 ", tasks_per_producer, " 个任务...");
    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&pool, tasks_per_producer]{
            for (int j = 0; j < tasks_per_producer; ++j) {
                pool.submit([]{ g_task_counter++; });
            }
        });
    }

    // 等待所有生产者线程完成任务提交
    for (auto& t : producers) {
        t.join();
    }
    print_safe("所有生产者已提交完毕。");

    print_safe("等待线程池完成所有任务 (优雅关闭)...");
    pool.destroy(false);
    print_safe("压力测试销毁完成。");

    int expected_count = num_producers * tasks_per_producer;
    if (g_task_counter.load() == expected_count) {
        print_safe("[OK] 压力测试通过，所有任务都已执行，最终计数: ", g_task_counter.load());
    } else {
        print_safe("[FAIL] 压力测试失败，预期计数 ", expected_count, ", 实际计数: ", g_task_counter.load());
    }
}


int main() 
{
    // 测试 1: 验证最基本的功能。
    // - 提交一个不需要返回值的"即发即忘"任务。
    // - 提交一个需要返回值的任务，并通过 std::future 来等待并获取结果。
    test_basic_and_future();

    // 测试 2: 验证两种不同的销毁模式。
    // - "优雅关闭" (destroy(false))：验证它会等待队列中所有剩余任务执行完毕。
    // - "立即关闭" (destroy(true))：验证它会清空任务队列，丢弃所有未开始执行的任务。
    test_shutdown_modes();

    // 测试 3: 验证线程池的健壮性和异常安全。
    // - 验证在工作线程中抛出的异常能被安全捕获，而不会使整个程序或线程池崩溃。
    // - 验证任务的异常可以通过 std::future 正确地传递给提交任务的主线程。
    test_exception_handling();

    // 测试 4: 验证线程池的生命周期管理是否灵活。
    // - 验证线程池在被 destroy() 之后，可以被重新 start() 并再次正常工作。
    test_resettable();

    // 测试 5: 验证高并发下的表现和正确性。
    // - 模拟多个“生产者”线程同时、大量地向线程池提交任务。
    // - 验证在锁竞争的情况下，所有任务最终是否都能被正确执行，没有任何丢失。
    test_stress();

    print_safe("\n所有测试已完成。");

    return 0;
}