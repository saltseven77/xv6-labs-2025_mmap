#include "kernel/types.h"
#include "user/user.h"

// 测试计数器（每个进程独立）
static int local_read_count = 0;
static int local_write_count = 0;

// 简单的读者操作
void simple_reader(int reader_id) {
    printf("Reader %d: acquiring read lock\n", reader_id);
    
    read_acquire();
    printf("Reader %d: got read lock, reading\n", reader_id);
    
    // 模拟读取时间
    for (volatile int j = 0; j < 50000; j++);
    local_read_count++;
    
    printf("Reader %d: releasing read lock\n", reader_id);
    read_release();
}

// 简单的写者操作
void simple_writer(int writer_id) {
    printf("Writer %d: acquiring write lock\n", writer_id);
    
    write_acquire();
    printf("Writer %d: got write lock, writing\n", writer_id);
    
    // 模拟写入时间
    for (volatile int j = 0; j < 100000; j++);
    local_write_count++;
    
    printf("Writer %d: releasing write lock\n", writer_id);
    write_release();
}

// 简单的并发测试
void test_simple_concurrent() {
    printf("\n=== Testing Simple Concurrent Access ===\n");
    
    int pid1, pid2;
    int status;
    
    // 启动一个读者
    if ((pid1 = fork()) == 0) {
        simple_reader(1);
        exit(0);
    }
    
    // 启动一个写者
    if ((pid2 = fork()) == 0) {
        simple_writer(1);
        exit(0);
    }
    
    // 等待两个进程完成
    wait(&status);
    wait(&status);
    
    printf("Simple concurrent test completed\n");
}

// 多读者测试
void test_multiple_readers() {
    printf("\n=== Testing Multiple Readers ===\n");
    
    int pid1, pid2;
    int status;
    
    // 启动两个读者
    if ((pid1 = fork()) == 0) {
        simple_reader(1);
        exit(0);
    }
    
    if ((pid2 = fork()) == 0) {
        simple_reader(2);
        exit(0);
    }
    
    // 等待两个读者完成
    wait(&status);
    wait(&status);
    
    printf("Multiple readers test completed\n");
}

// 写者优先测试
void test_writer_priority() {
    printf("\n=== Testing Writer Priority ===\n");
    
    int reader_pid, writer_pid;
    int status;
    
    // 启动一个长时间运行的读者
    if ((reader_pid = fork()) == 0) {
        printf("Long reader: acquiring read lock\n");
        read_acquire();
        printf("Long reader: got read lock, reading for long time\n");
        
        // 长时间读取
        for (volatile int j = 0; j < 500000; j++);
        
        printf("Long reader: releasing read lock\n");
        read_release();
        exit(0);
    }
    
    // 短暂延迟，让读者获取锁
    for (volatile int i = 0; i < 10000; i++);
    
    // 启动写者（应该等待）
    if ((writer_pid = fork()) == 0) {
        printf("Writer: acquiring write lock (should wait)\n");
        write_acquire();
        printf("Writer: got write lock, writing\n");
        
        for (volatile int j = 0; j < 100000; j++);
        
        printf("Writer: releasing write lock\n");
        write_release();
        exit(0);
    }
    
    // 等待两个进程完成
    wait(&status);
    wait(&status);
    
    printf("Writer priority test completed\n");
}

int main() {
    printf("=== Reader-Writer Synchronization Test ===\n");

    test_multiple_readers();
    test_simple_concurrent();
    test_writer_priority();
    
    printf("\nrwsynctest completed successfully!\n");
    return 0;
}
