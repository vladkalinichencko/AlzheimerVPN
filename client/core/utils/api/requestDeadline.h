#ifndef REQUESTDEADLINE_H
#define REQUESTDEADLINE_H

#include <QDateTime>
#include <QNetworkReply>
#include <QTimer>

namespace apiUtils
{
    // Aborts reply once the wall-clock deadline passes; 0 means no deadline.
    // The abort surfaces as OperationCanceledError, which maps to ApiConfigTimeoutError.
    inline void abortReplyAtDeadline(QNetworkReply *reply, qint64 deadlineMsecsSinceEpoch)
    {
        if (deadlineMsecsSinceEpoch <= 0) {
            return;
        }
        const qint64 remaining = qMax<qint64>(0, deadlineMsecsSinceEpoch - QDateTime::currentMSecsSinceEpoch());
        QTimer::singleShot(remaining, reply, [reply]() { reply->abort(); });
    }
}

#endif // REQUESTDEADLINE_H
