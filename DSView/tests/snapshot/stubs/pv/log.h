/* Test stub: LogicSnapshot only logs, and only reports errors. */
#ifndef DSVIEW_TESTS_SNAPSHOT_STUB_LOG_H
#define DSVIEW_TESTS_SNAPSHOT_STUB_LOG_H

#include <cstdio>

#define dsv_info(...) do {} while (0)
#define dsv_err(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)

#endif
