#ifndef APICONSTANTS_H
#define APICONSTANTS_H

namespace apiDefs
{

constexpr int requestTimeoutMsecs = 6 * 1000;
constexpr int proxyRequestTimeoutMsecs = 6 * 1000;
constexpr int proxyStorageTimeoutMsecs = 6 * 1000;
constexpr int proxyHealthTimeoutMsecs = 1000;

// Total time a connect-time config refresh may block, proxy fallbacks included.
constexpr int connectConfigRefreshDeadlineMsecs = 20 * 1000; // 20 secs

} // namespace apiDefs

#endif // APICONSTANTS_H
