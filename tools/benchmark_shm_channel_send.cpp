﻿#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <functional>
#include <limits>
#include <numeric>

#include <detail/libatbus_error.h>
#include "detail/libatbus_channel_export.h"

#ifdef max
#undef max
#endif

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("usage: %s <shm key> [max unit size] [shm size]\n", argv[0]);
        return 0;
    }

    using namespace atbus::channel;
    size_t max_n = 1024;
    if (argc > 2)
        max_n = (size_t)strtol(argv[2], NULL, 10);

    size_t buffer_len = 64 * 1024 * 1024; // 64MB
    if (argc > 3)
        buffer_len = (size_t)strtol(argv[3], NULL, 10);

    shm_channel* channel = NULL;
    key_t shm_key = (key_t)strtol(argv[1], NULL, 10);

    int res = shm_attach(shm_key, buffer_len, &channel, NULL);
    if (res < 0) {
        fprintf(stderr, "shm_attach failed, ret: %d\n", res);
        return res;
    }

    srand(static_cast<unsigned>(time(NULL)));

    size_t sum_send_len = 0;
    size_t sum_send_times = 0;
    size_t sum_send_full = 0;
    size_t sum_send_err = 0;
    size_t sum_seq = ((size_t)rand() << (sizeof(size_t) * 4));

    // 创建写线程
    std::thread* write_threads = new std::thread([&]{
        size_t* buf_pool = new size_t[max_n];
        int sleep_msec = 8;
        
        while(true) {
            size_t n = rand() % max_n; // 最大 4K-8K的包
            if (0 == n) n = 1; // 保证一定有数据包，保证收发次数一致

            for (size_t i = 0; i < n; ++ i) {
                buf_pool[i] = sum_seq;
            }

            int res = shm_send(channel, buf_pool, n * sizeof(size_t));

            if (res) {
                if (EN_ATBUS_ERR_BUFF_LIMIT == res) {
                    ++ sum_send_full;
                } else {
                    ++ sum_send_err;

                    std::pair<size_t, size_t> last_action = shm_last_action();
                    fprintf(stderr, "shm_send error, ret code: %d. start: %d, end: %d\n",
                        res, (int)last_action.first, (int)last_action.second
                    );
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_msec));
                if (sleep_msec < 32)
                    sleep_msec *= 2;
                    
            } else {
                ++ sum_send_times;
                sum_send_len += n * sizeof(size_t);
                ++ sum_seq;
                
                if (sleep_msec > 8)
                    sleep_msec /= 2;
            }
        }

        delete []buf_pool;
    });


    // 检查状态
    int secs = 0;
    char unit_desc[][4] = {"B", "KB", "MB", "GB"};
    size_t unit_devi[] = {1UL, 1UL<< 10, 1UL<< 20, 1UL<< 30};
    size_t unit_index = 0;

    while (true) {
        ++ secs;
        std::chrono::seconds dura( 60 );
        std::this_thread::sleep_for( dura );

        while ( sum_send_len / unit_devi[unit_index] > 1024 && unit_index < sizeof(unit_devi) / sizeof(size_t) - 1)
            ++ unit_index;

        while ( sum_send_len / unit_devi[unit_index] <= 1024 && unit_index > 0)
            -- unit_index;

        std::cout<< "[ RUNNING  ] NO."<< secs<< " m"<< std::endl<<
            "[ RUNNING  ] send("<< sum_send_times << " times, "<< (sum_send_len / unit_devi[unit_index]) << " "<< unit_desc[unit_index]<<") "<<
            "full "<< sum_send_full<< " times, err "<< sum_send_err<< " times"<<
            std::endl<< std::endl;
    }


    write_threads->join();
    delete write_threads;
}

