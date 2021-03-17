//
// Created by wanghaitao on 2021/2/25.
//

#ifndef CXXPRACTICE_LFTHREADPOOL_H
#define CXXPRACTICE_LFTHREADPOOL_H

#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <utility>

namespace yulan {
    template<int min_thread_num = 1, int max_thread_num = 10>
    class LfThreadPool {
    private:

        // 记录工作线程状态的工具结构
        struct ThreadInfo {
            std::thread t;
            bool running = false;
        };

    public:
        //  暴露出去的task类
        typedef std::function<void(void)> TaskType;

        template<typename Fun, typename ...Args>
        static inline TaskType make_task(Fun f, Args... args) {
            return std::bind(f, args...);
        }


    public:
        // 构造与析构
        LfThreadPool(TaskType pre_task, TaskType critical_task, TaskType follow_task);

        ~LfThreadPool();

        LfThreadPool(LfThreadPool &) = delete;

        LfThreadPool(LfThreadPool &&) = delete;

        LfThreadPool &operator=(LfThreadPool &) = delete;

        LfThreadPool &operator=(LfThreadPool &&) = delete;


    private:
        TaskType pre_task_;
        TaskType critical_task_;
        TaskType follow_task_;

        // 实现管程
        std::condition_variable cond_wait;  // 管程的条件变量
        std::mutex cond_mutex;              // 管程的锁
        int thread_num_allow_in;

        std::vector<ThreadInfo> threads;    // worker threads
        std::thread scheduler;              // scheduler thread
        bool shutdown;

        // 线程池运行状态记录
        int busy_thread_num;
        int live_thread_num;
        int wait_for_exit_num;

        inline void set_thread(int pool_index) {
            threads[pool_index].t = std::thread(
                    make_task(&LfThreadPool<min_thread_num, max_thread_num>::worker_func, this, pool_index));
            threads[pool_index].running = true;
        }

        void worker_func(int pool_index);

        void scheduler_func();
    };

    template<int min_thread_num, int max_thread_num>
    LfThreadPool<min_thread_num, max_thread_num>::LfThreadPool(TaskType pre_task,
                                                               TaskType critical_task,
                                                               TaskType follow_task):
            pre_task_(std::move(pre_task)),
            critical_task_(std::move(critical_task)),
            follow_task_(std::move(follow_task)),
            thread_num_allow_in(1),
            threads(max_thread_num),
            scheduler(&LfThreadPool<min_thread_num, max_thread_num>::scheduler_func, this),
            shutdown(false),
            busy_thread_num(min_thread_num),
            live_thread_num(min_thread_num),
            wait_for_exit_num(0) {
        for (int i = 0; i < min_thread_num; ++i) {
            set_thread(i);
        }
    }

    template<int min_thread_num, int max_thread_num>
    void LfThreadPool<min_thread_num, max_thread_num>::worker_func(int pool_index) {
        for (;;) {
            // 前置任务
            pre_task_();
            {
                // 进入临界区
                std::unique_lock<std::mutex> lk(cond_mutex);
                --busy_thread_num;
                while (thread_num_allow_in <= 0)
                    cond_wait.wait(lk);

                // 取得执行权
                --thread_num_allow_in;
                if (wait_for_exit_num > 0) {
                    --wait_for_exit_num;
                    threads[pool_index].t.detach();    // 自我了断
                    threads[pool_index].running = false;
                    --live_thread_num;
                    break;
                }

                // 临界任务
                critical_task_();

                // 退出临界区
                ++busy_thread_num;
                ++thread_num_allow_in;
            }
            cond_wait.notify_one(); // 先解锁后唤醒

            // 后续非临界处理任务
            follow_task_();
        }
    }

    template<int min_thread_num, int max_thread_num>
    void LfThreadPool<min_thread_num, max_thread_num>::scheduler_func() {
        while (!shutdown) {
            {
                std::lock_guard<std::mutex> lk(cond_mutex);

                // 扩充线程
                if (busy_thread_num * 5 > live_thread_num * 4) {
                    int add_num = busy_thread_num / 4;
                    for (int i = 0; i < max_thread_num && add_num > 0 && live_thread_num < max_thread_num; ++i) {
                        if (!threads[i].running) {
                            set_thread(i);
                            ++live_thread_num;
                            --add_num;
                        }
                    }
                }
                // 减少线程
                else if (busy_thread_num * 2 < live_thread_num) {
                    int quit_num = std::min(busy_thread_num, live_thread_num - min_thread_num);
                    wait_for_exit_num = quit_num;
                    thread_num_allow_in += quit_num;
                    cond_wait.notify_all();
                }
            }   // std::lock_guard<std::mutex> lk(cond_mutex)
            std::this_thread::yield();
        }
    }

    template<int min_thread_num, int max_thread_num>
    LfThreadPool<min_thread_num, max_thread_num>::~LfThreadPool() {
        shutdown = true;
        scheduler.join();
        // scheduler 已经退出，live_thread_num 不会改变


        {
            std::lock_guard<std::mutex> lk(cond_mutex);
            wait_for_exit_num = live_thread_num;
            thread_num_allow_in += wait_for_exit_num;
        }

        cond_wait.notify_all();
        for (int i = 0; i < max_thread_num; ++i) {
            if (threads[i].running) {
                threads[i].t.join();
            }
        }
    }
}   // namespace yulan


#endif //CXXPRACTICE_LFTHREADPOOL_H
