/* The matcher logs through xlog; the test drops the messages. */

#include <log/xlog.h>

xlog_writer *srd_log = 0;

int xlog_err(xlog_writer *wr, const char *format, ...) { (void)wr; (void)format; return 0; }
int xlog_warn(xlog_writer *wr, const char *format, ...) { (void)wr; (void)format; return 0; }
int xlog_info(xlog_writer *wr, const char *format, ...) { (void)wr; (void)format; return 0; }
int xlog_dbg(xlog_writer *wr, const char *format, ...) { (void)wr; (void)format; return 0; }
int xlog_detail(xlog_writer *wr, const char *format, ...) { (void)wr; (void)format; return 0; }
