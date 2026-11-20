#ifndef MANAGEDCONFIGPOLICY_H
#define MANAGEDCONFIGPOLICY_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace amnezia::managedConfig
{

    constexpr int defaultPollIntervalHours = 6;
    constexpr int minPollIntervalHours = 1;
    constexpr int maxPollIntervalHours = 168;

    enum class ReplyAction {
        Apply,
        KeepCurrent,
        RejectKey,
        TryNextBackend
    };

    struct ReplyDecision
    {
        ReplyAction action = ReplyAction::TryNextBackend;
        QString revision;
        QString config;
        int pollIntervalHours = 0;
    };

    QStringList parseBackendUrls(const QString &input);
    QStringList orderedBackendUrls(const QStringList &backendUrls, const QString &lastWorkingUrl);
    QUrl configEndpoint(const QString &backendUrl);

    ReplyDecision decideOnReply(int httpStatus, const QByteArray &body, const QString &currentRevision);

    int normalizedPollIntervalHours(int requestedHours);
    qint64 nextPollDelayMs(int intervalHours, double randomUnit);

} // namespace amnezia::managedConfig

#endif // MANAGEDCONFIGPOLICY_H
