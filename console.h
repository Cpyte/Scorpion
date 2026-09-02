#ifndef SCORPION_CONSOLE_H
#define SCORPION_CONSOLE_H

#include <stddef.h>

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3
#define LOG_PANIC 4

void console_init(void);
void console_putchar(char c);
void console_write(const char *s, size_t len);
void console_puts(const char *s);
void console_flush(void);

void log_message(int level, const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

void __attribute__((noreturn)) panic(const char *fmt, ...);

#endif
