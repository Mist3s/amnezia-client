#include "managedConfigPolicy.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace
{
    constexpr QLatin1String httpsScheme("https");
    constexpr QLatin1String configPath("/v1/config");

    constexpr QLatin1String replyRevisionKey("revision");
    constexpr QLatin1String replyConfigKey("config");
    constexpr QLatin1String replyPollIntervalHoursKey("poll_interval_hours");

    constexpr int httpOk = 200;
    constexpr int httpNotModified = 304;
    constexpr int httpUnauthorized = 401;
    constexpr int httpForbidden = 403;

    constexpr double pollJitterShare = 0.2;
    constexpr qint64 msInHour = 60 * 60 * 1000;

    QString normalizedBackendUrl(const QString &candidate)
    {
        const QString withScheme = candidate.contains("://") ? candidate : QString("%1://%2").arg(httpsScheme, candidate);
        const QUrl url(withScheme, QUrl::StrictMode);
        if (!url.isValid() || url.scheme() != httpsScheme || url.host().isEmpty()) {
            return QString();
        }

        QString normalized = url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
        while (normalized.endsWith('/')) {
            normalized.chop(1);
        }
        return normalized;
    }
}

namespace amnezia::managedConfig
{

    QStringList parseBackendUrls(const QString &input)
    {
        static const QRegularExpression separators("[\\s,;]+");

        QStringList urls;
        for (const QString &candidate : input.split(separators, Qt::SkipEmptyParts)) {
            const QString url = normalizedBackendUrl(candidate);
            if (!url.isEmpty() && !urls.contains(url)) {
                urls.append(url);
            }
        }
        return urls;
    }

    QStringList orderedBackendUrls(const QStringList &backendUrls, const QString &lastWorkingUrl)
    {
        QStringList ordered = backendUrls;
        const qsizetype lastWorkingIndex = ordered.indexOf(lastWorkingUrl);
        if (lastWorkingIndex > 0) {
            ordered.move(lastWorkingIndex, 0);
        }
        return ordered;
    }

    QUrl configEndpoint(const QString &backendUrl)
    {
        return QUrl(backendUrl + configPath);
    }

    ReplyDecision decideOnReply(int httpStatus, const QByteArray &body, const QString &currentRevision)
    {
        ReplyDecision decision;

        if (httpStatus == httpUnauthorized || httpStatus == httpForbidden) {
            decision.action = ReplyAction::RejectKey;
            return decision;
        }
        if (httpStatus == httpNotModified) {
            decision.action = ReplyAction::KeepCurrent;
            return decision;
        }
        if (httpStatus != httpOk) {
            return decision;
        }

        const QJsonObject reply = QJsonDocument::fromJson(body).object();
        decision.revision = reply.value(replyRevisionKey).toString();
        decision.config = reply.value(replyConfigKey).toString();
        decision.pollIntervalHours = reply.value(replyPollIntervalHoursKey).toInt();

        if (decision.revision.isEmpty() || decision.config.trimmed().isEmpty()) {
            return decision;
        }

        decision.action = decision.revision == currentRevision ? ReplyAction::KeepCurrent : ReplyAction::Apply;
        return decision;
    }

    int normalizedPollIntervalHours(int requestedHours)
    {
        if (requestedHours <= 0) {
            return defaultPollIntervalHours;
        }
        return qBound(minPollIntervalHours, requestedHours, maxPollIntervalHours);
    }

    qint64 nextPollDelayMs(int intervalHours, double randomUnit)
    {
        const double jitter = (qBound(0.0, randomUnit, 1.0) * 2.0 - 1.0) * pollJitterShare;
        return static_cast<qint64>(normalizedPollIntervalHours(intervalHours) * msInHour * (1.0 + jitter));
    }

} // namespace amnezia::managedConfig
