#include "kernel/types.h"
#include "user/user.h"

// 简化的共享数据
static int shared_value = 0;
static int read_count = 0;
static int write_count = 0;
static int active_readers = 0;
static int active_writers = 0;

// 读者操作：读取共享数据
int reader_operation(int reader_id, int iterations) {
    int errors = 0;
    
    for (int i = 0; i < iterations; i++) {
        // 获取读锁
        if (read_acquire() < 0) {
            printf("rwsynctest: reader %d failed to acquire read lock\n", reader_id);
            errors++;
            continue;
        }
        
        // 模拟读操作
        __sync_fetch_and_add(&active_readers, 1);
        int current_value = shared_value;
        int current_readers = active_readers;
        int current_writers = active_writers;
        
        // 验证读写互斥：读时不应有写者
        if (current_writers > 0) {
            printf("rwsynctest: ERROR - Reader %d found %d active writers!\n", 
                   reader_id, current_writers);
            errors++;
        }
        
        // 模拟读取时间
        for (volatile int j = 0; j < 1000; j++);
        
        __sync_fetch_and_add(&read_count, 1);
        __sync_fetch_and_sub(&active_readers, 1);
        
        printf("Reader %d: read value=%d, readers=%d, total_reads=%d\n", 
               reader_id, current_value, current_readers, read_count);
        
        // 释放读锁
        read_release();
        
        // 短暂休息
        for (volatile int j = 0; j < 500; j++);
    }
    
    return errors;
}

// 写者操作：修改共享数据
int writer_operation(int writer_id, int iterations) {
    int errors = 0;
    
    for (int i = 0; i < iterations; i++) {
        // 获取写锁（写者优先）
        if (write_acquire() < 0) {
            printf("rwsynctest: writer %d failed to acquire write lock\n", writer_id);
            errors++;
            continue;
        }
        
        // 模拟写操作
        __sync_fetch_and_add(&active_writers, 1);
        int current_readers = active_readers;
        int current_writers = active_writers;
        
        // 验证写者互斥：写时不应有其他读者或写者
        if (current_readers > 0) {
            printf("rwsynctest: ERROR - Writer %d found %d active readers!\n", 
                   writer_id, current_readers);
            errors++;
        }
        if (current_writers > 1) {
            printf("rwsynctest: ERROR - Writer %d found %d other active writers!\n", 
                   writer_id, current_writers - 1);
            errors++;
        }
        
        // 执行写操作
        int old_value = shared_value;
        shared_value = old_value + 1;
        
        // 模拟写入时间
        for (volatile int j = 0; j < 2000; j++);
        
        __sync_fetch_and_add(&write_count, 1);
        __sync_fetch_and_sub(&active_writers, 1);
        
        printf("Writer %d: wrote value %d->%d, total_writes=%d\n", 
               writer_id, old_value, shared_value, write_count);
        
        // 释放写锁
        write_release();
        
        // 短暂休息
        for (volatile int j = 0; j < 1000; j++);
    }
    
    return errors;
}

// 测试写者优先：当写者等待时，新读者应该被阻塞
void test_writer_priority() {
    printf("\n=== Testing Writer Priority ===\n");
    
    int reader_pid, writer_pid, late_reader_pid;
    int status;
    
    // 启动一个读者（持续读取）
    if ((reader_pid = fork()) == 0) {
        printf("Early reader starting...\n");
        reader_operation(1, 10);
        exit(0);
    }
    
    // 短暂延迟，让读者开始
    for (volatile int i = 0; i < 10000; i++);
    
    // 启动写者（应该等待读者完成）
    if ((writer_pid = fork()) == 0) {
        printf("Writer starting (should wait for reader)...\n");
        writer_operation(1, 3);
        exit(0);
    }
    
    // 再短暂延迟，让写者开始等待
    for (volatile int i = 0; i < 5000; i++);
    
    // 启动后来的读者（应该被写者阻塞）
    if ((late_reader_pid = fork()) == 0) {
        printf("Late reader starting (should be blocked by waiting writer)...\n");
        reader_operation(2, 5);
        exit(0);
    }
    
    // 等待所有进程完成
    wait(&status);
    wait(&status);
    wait(&status);
    
    printf("Writer priority test completed\n");
}

// 并发读者测试：多个读者应该能同时访问
void test_concurrent_readers() {
    printf("\n=== Testing Concurrent Readers ===\n");
    
    int pids[4];
    int status;
    
    // 启动多个读者
    for (int i = 0; i < 4; i++) {
        if ((pids[i] = fork()) == 0) {
            printf("Reader %d starting...\n", i + 1);
            reader_operation(i + 1, 5);
            exit(0);
        }
    }
    
    // 等待所有读者完成
    for (int i = 0; i < 4; i++) {
        wait(&status);
    }
    
    printf("Concurrent readers test completed\n");
}

// 读写混合测试
void test_mixed_operations() {
    printf("\n=== Testing Mixed Read/Write Operations ===\n");
    
    int pids[6];
    int status;
    
    // 启动2个写者和4个读者
    for (int i = 0; i < 2; i++) {
        if ((pids[i] = fork()) == 0) {
            printf("Writer %d starting...\n", i + 1);
            writer_operation(i + 1, 3);
            exit(0);
        }
    }
    
    for (int i = 2; i < 6; i++) {
        if ((pids[i] = fork()) == 0) {
            printf("Reader %d starting...\n", i - 1);
            reader_operation(i - 1, 4);
            exit(0);
        }
    }
    
    // 等待所有进程完成
    for (int i = 0; i < 6; i++) {
        wait(&status);
    }
    
    printf("Mixed operations test completed\n");
}

int main() {
    printf("=== Reader-Writer Synchronization Test ===\n");
    
    // 运行测试
    test_concurrent_readers();
    test_writer_priority();
    test_mixed_operations();
    
    // 输出最终统计
    printf("\n=== Final Statistics ===\n");
    printf("Total reads: %d\n", read_count);
    printf("Total writes: %d\n", write_count);
    printf("Final value: %d\n", shared_value);
    
    // 验证数据一致性
    if (shared_value == write_count) {
        printf("SUCCESS: Data consistency verified!\n");
    } else {
        printf("ERROR: Data inconsistency detected! Expected %d, got %d\n", 
               write_count, shared_value);
    }
    
    printf("rwsynctest completed\n");
    return 0;
}
