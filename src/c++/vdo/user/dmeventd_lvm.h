/* Logging functions that use dmeventd logging */
#define LOG_MESG(l, f, ln, e, ...) print_log(l, f, ln, e, ## __VA_ARGS__)
__attribute__((format(printf, 5, 6)))
static void print_log(int level, const char *file, int line,
		      int dm_errno_or_class, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  dm_event_log("vdo", level, file, line, dm_errno_or_class, format, ap);
  va_end(ap);
}

#define LOG_LINE(l, ...) LOG_MESG(l, __FILE__, __LINE__, 0, ## __VA_ARGS__)

#define _LOG_FATAL         0x0002
#define _LOG_ERR           0x0003
#define _LOG_WARN          0x0004
#define _LOG_NOTICE        0x0005
#define _LOG_INFO          0x0006
#define _LOG_DEBUG         0x0007

#define log_debug(...) LOG_LINE(_LOG_DEBUG, __VA_ARGS__)
#define log_info(...) LOG_LINE(_LOG_INFO, __VA_ARGS__)
#define log_notice(...) LOG_LINE(_LOG_NOTICE, __VA_ARGS__)
#define log_warn(...) LOG_LINE(_LOG_WARN, __VA_ARGS__)
#define log_error(...) LOG_LINE(_LOG_ERR, __VA_ARGS__)
#define log_fatal(...) LOG_LINE(_LOG_FATAL, __VA_ARGS__)
