#pragma once

// Consumes the arguments, so the parser's log-only parameters stay used.
template <typename... Args>
inline void logStub(const Args&...) {}

#define LOG_DBG(...) logStub(__VA_ARGS__)
#define LOG_INF(...) logStub(__VA_ARGS__)
#define LOG_ERR(...) logStub(__VA_ARGS__)
