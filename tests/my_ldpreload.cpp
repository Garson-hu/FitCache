#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif
#include <iostream>
#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include <vector>
#include <set>
#include <fstream>
#include <unistd.h>
#include <execinfo.h>
extern "C" {
    int (*real_open)(const char *pathname, int flags, ...) = nullptr;
    int (*real_open64)(const char *pathname, int flags, ...) = nullptr;
    FILE* (*real_fopen)(const char *pathname, const char *mode) = nullptr;

    int (*real_close)(int fd) = nullptr;

    ssize_t (*real_read)(int fd, void *buf, size_t count) = nullptr;
    ssize_t (*real_read64)(int fd, void *buf, size_t count) = nullptr;
    ssize_t (*real_readv)(int fd, const struct iovec *iov, int iovcnt) = nullptr;
    ssize_t (*real_pread)(int fd, void *buf, size_t count, off_t offset) = nullptr;

    size_t read64_count = 0;
    size_t readv_count = 0;
    size_t pread_count = 0;
    size_t open_count = 0;
    size_t fopen_count = 0;
    size_t read_count = 0;
    size_t close_count = 0;

    std::vector<long> open_times;
    std::vector<long> fopen_times;
    std::vector<long> close_times;

    std::set<int> unique_open_fds;
    std::set<int> unique_close_fds;
    std::set<FILE*> unique_fopen_files;

    std::vector<long> read_times;
    std::vector<long> read64_times;
    std::vector<long> readv_times;
    std::vector<long> pread_times;

    long get_duration(struct timespec start, struct timespec end) {
        return (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
    }

    
    int open(const char *pathname, int flags, ...) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_open) {
            real_open = (int (*)(const char*, int, ...)) dlsym(RTLD_NEXT, "open");
            if (!real_open) {
                fprintf(stderr, "Error in `dlsym` for open\n");
                return -1;
            }
        }

        va_list args;
        va_start(args, flags);
        int fd;
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = real_open(pathname, flags, mode);
        } else {
            fd = real_open(pathname, flags);
        }
        va_end(args);

        clock_gettime(CLOCK_MONOTONIC, &end);
        open_times.push_back(get_duration(start, end));
        printf("DEBUG_HU: MY_LD_PRELOAD: Open fd %d from pathname %s\n", fd, pathname);
        // print_backtrace();
        if (fd >= 0) {
            unique_open_fds.insert(fd);
            open_count++;
        } else {
            printf("DEBUG_HU: MY_LD_PRELOAD: Failed to open %s\n", pathname);
        }
        return fd;
    }

    int open64(const char *pathname, int flags, ...) {
        if (!real_open64) {
            real_open64 = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open64");
        }

        va_list args;
        va_start(args, flags);
        int fd;
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = real_open64(pathname, flags, mode);
        } else {
            fd = real_open64(pathname, flags);
        }
        va_end(args);

        printf("DEBUG_HU: MY_LD_PRELOAD: Open64 fd %d from pathname %s\n", fd, pathname);
        return fd;
    }

    FILE* fopen(const char *pathname, const char *mode) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_fopen) {
            real_fopen = (FILE* (*)(const char*, const char*)) dlsym(RTLD_NEXT, "fopen");
            if (!real_fopen) {
                fprintf(stderr, "Error in `dlsym` for fopen\n");
                return nullptr;
            }
        }

        FILE* file = real_fopen(pathname, mode);

        clock_gettime(CLOCK_MONOTONIC, &end);
        fopen_times.push_back(get_duration(start, end));
        printf("DEBUG_HU: MY_LD_PRELOAD: Fopen file %p from pathname %s\n", file, pathname);

        if (file) {
            unique_fopen_files.insert(file);
            fopen_count++;
        } else {
            printf("DEBUG_HU: MY_LD_PRELOAD: Failed to fopen %s\n", pathname);
        }
        return file;
    }

    ssize_t read(int fd, void *buf, size_t count) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_read) {
            real_read = (ssize_t (*)(int, void*, size_t)) dlsym(RTLD_NEXT, "read");
            if (!real_read) {
                fprintf(stderr, "Error in `dlsym` for read\n");
                return -1;
            }
        }
        ssize_t ret = real_read(fd, buf, count);

        clock_gettime(CLOCK_MONOTONIC, &end);
        read_times.push_back(get_duration(start, end));
        printf("DEBUG_HU: MY_LD_PRELOAD: Read fd %d\n", fd);
        read_count++;
        return ret;
    }

    ssize_t read64(int fd, void *buf, size_t count) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_read64) {
            real_read64 = (ssize_t (*)(int, void*, size_t)) dlsym(RTLD_NEXT, "read64");
            if (!real_read64) {
                fprintf(stderr, "Error in `dlsym` for read64\n");
                return -1;
            }
        }
        ssize_t ret = real_read64(fd, buf, count);

        clock_gettime(CLOCK_MONOTONIC, &end);
        read64_times.push_back(get_duration(start, end));
        printf("DEBUG_HU: MY_LD_PRELOAD: Read64 fd %d\n", fd);
        read64_count++;
        return ret;
    }

    ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_readv) {
            real_readv = (ssize_t (*)(int, const struct iovec*, int)) dlsym(RTLD_NEXT, "readv");
            if (!real_readv) {
                fprintf(stderr, "Error in `dlsym` for readv\n");
                return -1;
            }
        }
        ssize_t ret = real_readv(fd, iov, iovcnt);

        clock_gettime(CLOCK_MONOTONIC, &end);
        readv_times.push_back(get_duration(start, end));
        readv_count++;
        return ret;
    }

    ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_pread) {
            real_pread = (ssize_t (*)(int, void*, size_t, off_t)) dlsym(RTLD_NEXT, "pread");
            if (!real_pread) {
                fprintf(stderr, "Error in `dlsym` for pread\n");
                return -1;
            }
        }
        ssize_t ret = real_pread(fd, buf, count, offset);
        clock_gettime(CLOCK_MONOTONIC, &end);
        pread_times.push_back(get_duration(start, end));
        printf("DEBUG_HU: MY_LD_PRELOAD: Pread %ld bytes from fd %d\n", count, fd);
        pread_count++;
        return ret;
    }

    int close(int fd) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (!real_close) {
            real_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
            if (!real_close) {
                fprintf(stderr, "Error in `dlsym` for close\n");
                return -1;
            }
        }
        int ret = real_close(fd);

        clock_gettime(CLOCK_MONOTONIC, &end);
        close_times.push_back(get_duration(start, end));
        // print_backtrace();
        printf("DEBUG_HU: MY_LD_PRELOAD: Close fd %d\n", fd);
        close_count++;
        unique_close_fds.insert(fd);
        return ret;
    }

    __attribute__((destructor)) void write_results() {
        std::ofstream outfile("operation_stats.txt"); 

        outfile << "Open operations: " << open_count << "\n";
        for (auto time : open_times) outfile << time << " ns\n";

        outfile << "\nFopen operations: " << fopen_count << "\n";
        for (auto time : fopen_times) outfile << time << " ns\n";

        outfile << "\nRead operations: " << read_count << "\n";
        for (auto time : read_times) outfile << time << " ns\n";

        outfile << "\nClose operations: " << close_count << "\n";
        for (auto time : close_times) outfile << time << " ns\n";

        outfile << "\nUnique Open FDs: " << unique_open_fds.size() << ", FDs: ";
        for (auto fd : unique_open_fds) outfile << fd << " ";
        outfile << "\n";

        outfile << "Unique Close FDs: " << unique_close_fds.size() << ", FDs: ";
        for (auto fd : unique_close_fds) outfile << fd << " ";
        outfile << "\n";

        outfile << "Unique Fopen files: " << unique_fopen_files.size() << ", Files: ";
        for (auto file : unique_fopen_files) outfile << file << " ";
        outfile << "\n";

        outfile << "\nRead64 operations: " << read64_count << "\n";
        for (auto time : read64_times) outfile << time << " ns\n";

        outfile << "\nReadv operations: " << readv_count << "\n";
        for (auto time : readv_times) outfile << time << " ns\n";

        outfile << "\nPread operations: " << pread_count << "\n";
        for (auto time : pread_times) outfile << time << " ns\n";

        outfile.close();
    }
}
