//
// Created by wanghaitao on 2021/2/20.
//

#include <iostream>
#include <thread>
#include "thread_pool.hpp"


const int Work_Range = 200000;

bool count[Work_Range];


void process(int i) {
    count[i] = true;
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


int main() {
    {
        yulan::ThreadPool<5, 20> pool;

        for (int i = 0; i < Work_Range; ++i) {
            pool.add_task(yulan::make_task(process, i));
        }
    }
    for (int i = 0; i < Work_Range; ++i) {
        if (!count[i]) {
            std::cout << i << "error" << std::endl;
        }
    }
    std::cout << "ok" << std::endl;
    return 0;
}
