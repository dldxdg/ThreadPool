#include <iostream>
#include <chrono>   // 用于 std::chrono::seconds
#include <numeric>  // 用于 std::accumulate
#include "ThreadPool.hpp"

// 用于测试的全局原子计数器，保证多线程修改安全
std::atomic<int> g_task_count{0};


// --- 测试用例函数 ---

// 任务1：简单的任务
void simple_task() {
    std::cout << "  > simple_task anan 执行中... (线程ID: " << std::this_thread::get_id() << ")\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    g_task_count++;
}

// 任务2：带参数和返回值的任务
int sum_task(int start, int end) {
    std::cout << "  > sum_task anan 执行中... (线程ID: " << std::this_thread::get_id() << ")\n";
    int sum = 0;
    for (int i = start; i <= end; ++i) {
        sum += i;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return sum;
}

// 任务3：会抛出异常的任务
void exception_task() {
    std::cout << "  > exception_task anan 执行中，准备抛出异常... (线程ID: " << std::this_thread::get_id() << ")\n";
    throw std::runtime_error("这是一个测试异常");
}

// --- 测试场景 ---

// 场景1：测试基本提交和 future 返回值
void test_basic_and_future() {
    std::cout << "\n--- [测试场景 1: 基本任务提交与 Future 返回值] ---\n";
    xdg::ThreadPool pool(2, 4); // 2个线程，队列容量4
    pool.start();
    std::cout << "线程池已启动...\n";

    // 1. 提交一个简单任务
    std::cout << "提交 simple_task (无返回值)...\n";
    pool.submit(simple_task);

    // 2. 提交一个带返回值的任务
    std::cout << "提交 sum_task (1 到 100)...\n";
    std::future<int> future_sum = pool.submitTask(sum_task, 1, 100);

    // 3. 在主线程做点别的事情
    std::cout << "主线程等待 future 结果...\n";
    
    // 4. 获取任务结果 (get() 会阻塞直到结果准备好)
    int sum_result = future_sum.get();
    std::cout << "sum_task 结果已获取: " << sum_result << " (预期: 5050)\n";
    if (sum_result == 5050) {
        std::cout << "[OK] 返回值正确。\n";
    } else {
        std::cout << "[FAIL] 返回值错误。\n";
    }

    // 等待 simple_task 执行完毕
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (g_task_count.load() == 1) {
        std::cout << "[OK] fire-and-forget 任务已执行。\n";
    } else {
        std::cout << "[FAIL] fire-and-forget 任务未执行。\n";
    }

    pool.destroy();
    std::cout << "线程池已销毁。\n";
}

// 场景2：测试有界队列的阻塞行为
void test_bounded_queue() {
    std::cout << "\n--- [测试场景 2: 有界队列阻塞] ---\n";
    // 1个线程，队列容量1。这样很容易测试阻塞
    xdg::ThreadPool pool(1, 1);
    pool.start();
    std::cout << "线程池已启动 (1个线程, 队列容量1)...\n";

    // 1. 提交一个耗时2秒的任务，它会立刻被执行，占满线程
    std::cout << "提交一个耗时2秒的任务...\n";
    pool.submit([]{
        std::cout << "  > 长任务开始... (线程ID: " << std::this_thread::get_id() << ")\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "  > 长任务结束。\n";
    });

    // 2. 再提交一个任务，它会进入队列，占满队列
    std::cout << "提交第二个任务 (将会填满队列)...\n";
    pool.submit(simple_task);

    // 3. 主线程尝试提交第三个任务，此时线程和队列都满了，submit 调用应该会阻塞
    std::cout << "主线程准备提交第三个任务，预期将在此阻塞...\n";
    auto start_time = std::chrono::high_resolution_clock::now();
    
    pool.submit([]{
        std::cout << "  > 第三个任务终于被执行了... (线程ID: " << std::this_thread::get_id() << ")\n";
        g_task_count++;
    });

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "主线程阻塞结束，耗时: " << duration.count() << "ms\n";

    if (duration.count() > 1000) {
        std::cout << "[OK] 生产者线程 (主线程) 成功被阻塞。\n";
    } else {
        std::cout << "[FAIL] 生产者线程未按预期阻塞。\n";
    }

    pool.destroy();
    std::cout << "线程池已销毁。\n";
}

// 场景3：测试异常处理
void test_exception_handling() {
    std::cout << "\n--- [测试场景 3: 异常处理] ---\n";
    xdg::ThreadPool pool(2);
    pool.start();
    std::cout << "线程池已启动...\n";
    
    // 1. 测试 fire-and-forget 任务的异常 (我们应该能在 stderr 看到 worker_loop 的打印)
    std::cout << "提交一个会抛出异常的 fire-and-forget 任务...\n";
    pool.submit(exception_task);

    // 2. 测试带 future 任务的异常
    std::cout << "提交一个会抛出异常的 future 任务...\n";
    std::future<void> future_exc = pool.submitTask(exception_task);

    try {
        std::cout << "主线程调用 future.get()，预期会捕获到异常...\n";
        future_exc.get();
    } catch (const std::runtime_error& e) {
        std::cout << "[OK] 成功在主线程捕获到异常: " << e.what() << "\n";
    } catch (...) {
        std::cout << "[FAIL] 捕获到未知异常。\n";
    }
    
    // 给第一个任务一点时间，确保它的异常信息能被打印出来
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    pool.destroy();
    std::cout << "线程池已销毁。\n";
}


// 场景4：测试重复启停
void test_resettable() {
    std::cout << "\n--- [测试场景 4: 重复启停] ---\n";
    xdg::ThreadPool pool(1);
    
    std::cout << "第一次启动线程池...\n";
    pool.start();
    auto future1 = pool.submitTask([]{ return 42; });
    int res1 = future1.get();
    std::cout << "第一次运行结果: " << res1 << " (预期: 42)\n";
    if (res1 == 42) std::cout << "[OK] 第一次运行正常。\n";
    else std::cout << "[FAIL] 第一次运行失败。\n";
    pool.destroy();
    std::cout << "第一次销毁完成。\n";

    std::cout << "\n第二次启动同一个线程池...\n";
    pool.start();
    auto future2 = pool.submitTask([]{ return 99; });
    int res2 = future2.get();
    std::cout << "第二次运行结果: " << res2 << " (预期: 99)\n";
    if (res2 == 99) std::cout << "[OK] 第二次运行正常。\n";
    else std::cout << "[FAIL] 第二次运行失败。\n";
    pool.destroy();
    std::cout << "第二次销毁完成。\n";
}


int main() {

    auto start_time = std::chrono::high_resolution_clock::now(); // 开始计时

    test_basic_and_future(); // 测试基础功能和 future
    test_bounded_queue(); // 测试有界队列的阻塞行为
    test_exception_handling(); // 测试异常处理
    test_resettable(); // 测试重复启停

    auto end_time = std::chrono::high_resolution_clock::now();

    std::cout << "\n所有测试已完成。\n";
    std::cout << "总耗时: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() << "ms\n";

    return 0;
}