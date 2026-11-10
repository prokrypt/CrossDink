#pragma once

// Consumes the arguments, so the parser's log-only parameters stay used.
template <typename... Args>
inline void logStub(const Args&...) {}

// LOG_ERR calls since start; the sample pack test expects none.
inline int gLogErrors = 0;

#define LOG_DBG(...) logStub(__VA_ARGS__)
#define LOG_INF(...) logStub(__VA_ARGS__)
#define LOG_ERR(...) (++gLogErrors, logStub(__VA_ARGS__))
