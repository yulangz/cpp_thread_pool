//
// Created by wanghaitao on 2021/2/20.
//

#ifndef CXX_THREAD_POOL_THREAD_POOL_HPP
#define CXX_THREAD_POOL_THREAD_POOL_HPP

#include <array>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unistd.h>

namespace yulan {
    typedef std::function<void(void)> TaskType;

    /**
     * task queue
     * @tparam Q_Size max size of buffer queue
     * @tparam TaskType task type
     */
    template<int Q_Size = 100, typename TaskType=TaskType>
    class TaskQueue {
    public:
        TaskQueue() : task_queue(),
                      mutex(),
                      wait_for_queue_empty(),
                      wait_for_queue_full() {

        }

        /**
         * put a task into queue
         * thread safe, and blocking the calling thread when the task queue is full
         * @param task a TaskType object put into the queue
         */
        template<typename _TaskType=TaskType>
        // use for perfectForward
        void put(_TaskType &&task) {
            {
                std::unique_lock<std::mutex> lk(mutex);
                while (task_queue.size() >= Q_Size)
                    wait_for_queue_empty.wait(lk);
                task_queue.push(std::forward<TaskType>(task));
            }   // unlock before notify
            wait_for_queue_full.notify_one();
        }

        /**
         * get a task from task queue
         * thread safe, and blocking the calling thread when the task queue is empty
         * @return a task
         */
        TaskType get() {
            TaskType tmp;
            {
                std::unique_lock<std::mutex> lk(mutex);
                while (task_queue.empty())
                    wait_for_queue_full.wait(lk);
                tmp = task_queue.front();
                task_queue.pop();
            }
            wait_for_queue_empty.notify_one();
            return tmp;
        }

        bool empty() {
            // TODO 这里有个小问题，就是有可能task_queue为空，但是wait_for_queue_empty还有进程阻塞着
            std::lock_guard<std::mutex> lk(mutex);
            return task_queue.empty();
        }

        size_t size() {
            std::lock_guard<std::mutex> lk(mutex);
            return task_queue.size();
        }

    private:
        std::queue<TaskType> task_queue;
        std::condition_variable wait_for_queue_empty;   // producer block here
        std::condition_variable wait_for_queue_full;    // consumer block here
        std::mutex mutex;
    };

    /**
     * make a callable to a task object.
     * @tparam Fun callable
     * @tparam Args any args
     * @return a task object
     */
    template<typename Fun, typename ...Args>
    TaskType make_task(Fun f, Args... args) {
        return std::bind(f, args...);
    }

    /**
     * a thread pool class
     * @tparam min_thread_num
     * @tparam max_thread_num
     */
    template<int min_thread_num, int max_thread_num>
    class ThreadPool {
        struct ThreadInfo {
            std::thread t;
            bool running = false;
        };
    public:

        ThreadPool() : task_queue(),
                       scheduler(&ThreadPool<min_thread_num, max_thread_num>::scheduler_func, this),
                       shutdown(false),
                       busy_thread_num(0),
                       live_thread_num(min_thread_num),
                       wait_for_exit_num(0),
                       threads(max_thread_num) {
            for (int i = 0; i < min_thread_num; ++i) {
                set_thread(i);
            }
        }

        ThreadPool(ThreadPool &) = delete;

        ThreadPool(ThreadPool &&) = delete;

        ThreadPool &operator=(ThreadPool &) = delete;

        ThreadPool &operator=(ThreadPool &&) = delete;

        ~ThreadPool();

        /**
         * add a task to the thread pool
         * @param task A task object
         */
        inline void add_task(TaskType task) {
            task_queue.put(task);
        }

    private:
        TaskQueue<max_thread_num> task_queue;
        std::vector<ThreadInfo> threads;    // worker threads
        std::thread scheduler;              // scheduler thread
        bool shutdown;
        std::atomic<int> busy_thread_num;
        std::atomic<int> live_thread_num;   // TODO 不用互斥（只有scheduler使用）
        std::atomic<int> wait_for_exit_num;

        /**
         * scheduler thread
         */
        void scheduler_func();

        /**
         * worker thread
         * @param pool_index the index of this thread in the threads array
         */
        void worker_func(int pool_index);

        /**
         * add a worker thread to the thread pool
         * @param pool_index the index of this thread in the threads array
         */
        void set_thread(int pool_index) {
            threads[pool_index].t = std::thread(
                    make_task(&ThreadPool<min_thread_num, max_thread_num>::worker_func, this, pool_index));
            threads[pool_index].running = true;
        }
    };

    template<int min_thread_num, int max_thread_num>
    void ThreadPool<min_thread_num, max_thread_num>::worker_func(int pool_index) {
        while (true) {
            auto task = task_queue.get();
            ++busy_thread_num;
            task();
            --busy_thread_num;
            if (wait_for_exit_num > 0) {
                --wait_for_exit_num;
                threads[pool_index].t.detach();    // 自我了断
                threads[pool_index].running = false;
                --live_thread_num;
                break;
            }
        }
    }

    template<int min_thread_num, int max_thread_num>
    void ThreadPool<min_thread_num, max_thread_num>::scheduler_func() {
        while (!shutdown) {
            int busy_num = busy_thread_num;
            int live_num = live_thread_num;
            int queue_size = task_queue.size();

            // 扩充线程
            if (queue_size > busy_num && busy_num + queue_size > live_num) {
                int add_num = std::min(busy_num, queue_size);
                for (int i = 0; i < max_thread_num && add_num > 0 && live_thread_num < max_thread_num; ++i) {
                    if (!threads[i].running) {
                        set_thread(i);
                        ++live_thread_num;
                        --add_num;
                    }
                }
            }
                // 减少线程
            else if (busy_num * 2 < live_num) {
                int quit_num = std::min(busy_num, live_num - min_thread_num);
                wait_for_exit_num = quit_num;
                while (quit_num--) {
                    task_queue.put([]() {});     // 激活睡眠线程
                }
            }
            std::this_thread::yield();
        }
    }

    template<int min_thread_num, int max_thread_num>
    ThreadPool<min_thread_num, max_thread_num>::~ThreadPool() {
        shutdown = true;
        scheduler.join();
        // scheduler 已经退出，live_thread_num 不会改变
        // 先完成工作
        while (!task_queue.empty())
            std::this_thread::yield();
        int live_num = live_thread_num;
        wait_for_exit_num = live_num;
        for (int i = 0; i < live_num; ++i) {
            task_queue.put([]() {});
        }
        for (int i = 0; i < max_thread_num; ++i) {
            if (threads[i].running) {
                threads[i].t.join();
            }
        }
    }
}


#endif //CXX_THREAD_POOL_THREAD_POOL_HPP
