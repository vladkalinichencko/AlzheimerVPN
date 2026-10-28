#ifndef APICONSTANTS_H
#define APICONSTANTS_H

namespace apiDefs
{

constexpr int requestTimeoutMsecs = 12 * 1000; // 12 secs

// Total time a connect-time config refresh may block, proxy fallbacks included.
constexpr int connectConfigRefreshDeadlineMsecs = 20 * 1000; // 20 secs

} // namespace apiDefs

#endif // APICONSTANTS_H
