// Stand-in for the Android header so PedalReceiver.cpp builds and runs on a desktop for testing.
#pragma once
#include <cstdarg>
#include <cstdio>
#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_ERROR 6
inline int __android_log_print(int, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); return 0;
}
