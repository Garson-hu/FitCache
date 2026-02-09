#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define FILE_COUNT 128
#define EPOCHS 30

// 数据结构用于记录时间差
double open_deltas[FILE_COUNT][EPOCHS];
double open_close_deltas[FILE_COUNT][EPOCHS];

// 数据结构用于记录每个epoch的总运行时间
double open_epoch_times[EPOCHS];
double open_close_epoch_times[EPOCHS];

// 生成128个4MB的文件
void generate_files_open_close(uint32_t filecount){
    char tfn[256];
    /* 4MB contiguous buffer in bytes */
    char buffer[4096000];
    int testfile = 0;

    /* Seed random num gen */
    srand(time(NULL));

    for (int lcv = 0; lcv < filecount; lcv++)
    {
        snprintf(tfn, sizeof(tfn), "./testfile.%d.4096000.%d", getpid(), lcv);

        if ((testfile = open64(tfn, O_WRONLY | O_CREAT | O_TRUNC,
                        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1)
        {
            perror("Cannot open output file\n"); exit(1);
        }

        // 填充缓冲区
        memset(buffer, 'A', sizeof(buffer));
        write(testfile, (void *)buffer, sizeof(buffer));

        fclose(testfile);
    }
}

void generate_files_fopen_fclose(uint32_t filecount) {
    char tfn[256];
    /* 4MB contiguous buffer in bytes */
    char buffer[4096000];
    FILE *testfile;

    /* Seed random num gen */
    srand(time(NULL));

    for (int lcv = 0; lcv < filecount; lcv++) {
        snprintf(tfn, sizeof(tfn), "./testfile.%d.4096000.%d", getpid(), lcv);

        if ((testfile = fopen(tfn, "wb")) == NULL) {
            perror("Cannot open output file\n");
            exit(1);
        }

        // 填充缓冲区
        memset(buffer, 'A', sizeof(buffer));
        fwrite(buffer, sizeof(char), sizeof(buffer), testfile);

        // fclose(testfile);
    }
}

// 清理生成的文件
void cleanup_files(uint32_t filecount){
    char tfn[256];
   
    for (int lcv = 0; lcv < filecount; lcv++)
    {
        snprintf(tfn, sizeof(tfn), "./testfile.%d.4096000.%d", getpid(), lcv);
        unlink(tfn);
    }
}

// 测试组1：仅拦截 open()
void test_open_only(uint32_t filecount, uint32_t epochs){
    char tfn[256];
    int testfile = 0;

    for (int epoch = 0; epoch < epochs; epoch++) {
        struct timeval epoch_start, epoch_end;
        gettimeofday(&epoch_start, NULL);

        for (int lcv = 0; lcv < filecount; lcv++) {
            snprintf(tfn, sizeof(tfn), "./testfile.%d.4096000.%d", getpid(), lcv);

            struct timeval start, end;
            gettimeofday(&start, NULL);

            // 打开文件
            testfile = open(tfn, O_RDONLY);
            if (testfile == -1){
                perror("Cannot open output file\n"); exit(1);
            }

            gettimeofday(&end, NULL);

            // & with close but without LD_PRELOAD
            if (close(testfile) == -1){
                perror("Cannot close output file\n"); exit(1);
            }

            // close(testfile);
            // 计算 open 时间差
            double delta = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;
            open_deltas[lcv][epoch] = delta;

            // 不进行 close 操作
        }

        gettimeofday(&epoch_end, NULL);
        double elapsed = (epoch_end.tv_sec - epoch_start.tv_sec) + (epoch_end.tv_usec - epoch_start.tv_usec) / 1e6;
        open_epoch_times[epoch] = elapsed;
        printf("Test Open Only - Epoch %d: Total Elapsed Time: %.6f seconds\n", epoch+1, elapsed);
    }
}


// 测试组2：拦截 open() 和 close()
void test_open_close(uint32_t filecount, uint32_t epochs){
    char tfn[256];
    int testfile = 0;

    for (int epoch = 0; epoch < epochs; epoch++) {
        struct timeval epoch_start, epoch_end;
        gettimeofday(&epoch_start, NULL);

        for (int lcv = 0; lcv < filecount; lcv++) {
            snprintf(tfn, sizeof(tfn), "./testfile.%d.4096000.%d", getpid(), lcv);

            struct timeval start, end, close_start, close_end, open_start, open_close;
            gettimeofday(&start, NULL);

            // 打开文件
            gettimeofday(&open_start, NULL);
            testfile = open(tfn, O_RDONLY);
            if (testfile == -1){
                perror("Cannot open output file\n"); exit(1);
            }
            gettimeofday(&open_close, NULL);

            double delta_open = (open_close.tv_sec - open_start.tv_sec) + (open_close.tv_usec - open_start.tv_usec) / 1e6;
            printf("Test Open and Close - Epoch %d: Open w/o LDPRELOAD Time: %.6f seconds\n", epoch+1, delta_open);

            gettimeofday(&close_start, NULL);
            // 关闭文件
            if (close(testfile) == -1){
                perror("Cannot close output file\n"); exit(1);
            }
            gettimeofday(&close_end, NULL);
            
            double delta_close = (close_end.tv_sec - close_start.tv_sec) + (close_end.tv_usec - close_start.tv_usec) / 1e6;
            printf("Test Open and Close - Epoch %d: Close w/o LDPRELOAD Time: %.6f seconds\n", epoch+1, delta_close);

            gettimeofday(&end, NULL);

            // 计算 open + close 时间差
            double delta = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;
            open_close_deltas[lcv][epoch] = delta;
        }

        gettimeofday(&epoch_end, NULL);
        double elapsed = (epoch_end.tv_sec - epoch_start.tv_sec) + (epoch_end.tv_usec - epoch_start.tv_usec) / 1e6;
        open_close_epoch_times[epoch] = elapsed;
        // printf("Test Open and Close - Epoch %d: Total Elapsed Time: %.6f seconds\n", epoch+1, elapsed);
    }
}

// 将 open_deltas 写入 CSV 文件
void write_open_deltas_to_file(const char* filename){
    FILE *fp = fopen(filename, "w");
    if (!fp){
        perror("Failed to open open_deltas.csv for writing\n");
        exit(1);
    }

    // 写入头部
    fprintf(fp, "Epoch,FileID,Delta\n");

    for (int epoch = 0; epoch < EPOCHS; epoch++) {
        for (int file = 0; file < FILE_COUNT; file++) {
            fprintf(fp, "%d,%d,%.6f\n", epoch+1, file, open_deltas[file][epoch]);
        }
    }

    fclose(fp);
}

// 将 open_close_deltas 写入 CSV 文件
void write_open_close_deltas_to_file(const char* filename){
    FILE *fp = fopen(filename, "w");
    if (!fp){
        perror("Failed to open open_close_deltas.csv for writing\n");
        exit(1);
    }

    // 写入头部
    fprintf(fp, "Epoch,FileID,Delta\n");

    for (int epoch = 0; epoch < EPOCHS; epoch++) {
        for (int file = 0; file < FILE_COUNT; file++) {
            fprintf(fp, "%d,%d,%.6f\n", epoch+1, file, open_close_deltas[file][epoch]);
        }
    }

    fclose(fp);
}

// 将 open_epoch_times 写入 CSV 文件
void write_open_epoch_times_to_file(const char* filename){
    FILE *fp = fopen(filename, "w");
    if (!fp){
        perror("Failed to open open_epoch_times.csv for writing\n");
        exit(1);
    }

    // 写入头部
    fprintf(fp, "Epoch,TotalElapsedTime\n");

    for (int epoch = 0; epoch < EPOCHS; epoch++) {
        fprintf(fp, "%d,%.6f\n", epoch+1, open_epoch_times[epoch]);
    }

    fclose(fp);
}

// 将 open_close_epoch_times 写入 CSV 文件
void write_open_close_epoch_times_to_file(const char* filename){
    FILE *fp = fopen(filename, "w");
    if (!fp){
        perror("Failed to open open_close_epoch_times.csv for writing\n");
        exit(1);
    }

    // 写入头部
    fprintf(fp, "Epoch,TotalElapsedTime\n");

    for (int epoch = 0; epoch < EPOCHS; epoch++) {
        fprintf(fp, "%d,%.6f\n", epoch+1, open_close_epoch_times[epoch]);
    }

    fclose(fp);
}

int main(int argc, char **argv){

    printf("Starting test program:\n");
    fflush(stdout);

    // 生成128个4MB的文件
    generate_files_fopen_fclose(FILE_COUNT);

    // 进行测试组1：仅拦截 open()
    printf("Starting Test 1: Open Only\n");
    fflush(stdout);
    test_open_only(FILE_COUNT, EPOCHS);

    // 写入 open_deltas 到 CSV 文件
    write_open_deltas_to_file("open_deltas.csv");
    printf("Open Only test results saved to open_deltas.csv\n");
    fflush(stdout);

    // 写入 open_epoch_times 到 CSV 文件
    write_open_epoch_times_to_file("open_epoch_times.csv");
    printf("Open Only epoch times saved to open_epoch_times.csv\n");
    fflush(stdout);

    // 进行测试组2：拦截 open() 和 close()
    // printf("Starting Test 2: Open and Close\n");
    // fflush(stdout);
    // test_open_close(FILE_COUNT, EPOCHS);

    // // 写入 open_close_deltas 到 CSV 文件
    // write_open_close_deltas_to_file("open_close_deltas.csv");
    // printf("Open and Close test results saved to open_close_deltas.csv\n");
    // fflush(stdout);

    // // 写入 open_close_epoch_times 到 CSV 文件
    // write_open_close_epoch_times_to_file("open_close_epoch_times.csv");
    // printf("Open and Close epoch times saved to open_close_epoch_times.csv\n");
    // fflush(stdout);

    // 清理生成的文件
    // cleanup_files(FILE_COUNT);

    printf("Ending test program\n");
    fflush(stdout);
    return 0;
}
