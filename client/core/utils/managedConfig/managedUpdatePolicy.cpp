#include "managedUpdatePolicy.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QVersionNumber>

namespace
{
    constexpr QLatin1String httpsScheme("https");
    constexpr QLatin1String updatePath("/v1/app_update");

    constexpr QLatin1String replyUpdateAvailableKey("update_available");
    constexpr QLatin1String replyVersionKey("version");
    constexpr QLatin1String replyUrlKey("url");
    constexpr QLatin1String replySha256Key("sha256");
    constexpr QLatin1String replyReleaseDateKey("release_date");
    constexpr QLatin1String replyChangesKey("changes");
    constexpr QLatin1String changesNewKey("new");
    constexpr QLatin1String changesImprovedKey("improved");
    constexpr QLatin1String changesFixedKey("fixed");

    constexpr int httpOk = 200;
    constexpr int httpUnauthorized = 401;
    constexpr int httpForbidden = 403;

    bool isSha256Hex(const QString &value)
    {
        static const QRegularExpression pattern("^[0-9a-f]{64}$");
        return pattern.match(value).hasMatch();
    }

    QStringList changeLines(const QJsonValue &value)
    {
        QStringList lines;
        const QJsonArray items = value.toArray();
        for (const QJsonValue &item : items) {
            const QString line = item.toString().simplified();
            if (!line.isEmpty()) {
                lines.append(line);
            }
        }
        return lines;
    }
}

namespace amnezia::managedUpdate
{

    bool UpdateOffer::isValid() const
    {
        return !version.isEmpty() && fileUrl.isValid() && isSha256Hex(sha256);
    }

    QUrl updateEndpoint(const QString &backendUrl, const managedConfig::DeviceDescription &device)
    {
        QUrlQuery query;
        managedConfig::describeDevice(query, device);

        QUrl url(backendUrl + updatePath);
        url.setQuery(query);
        return url;
    }

    QUrl fileUrl(const QString &backendUrl, const QString &filePath)
    {
        const QUrl base(backendUrl + '/');
        const QUrl resolved = base.resolved(QUrl(filePath));

        const bool isSameOrigin =
                resolved.scheme() == httpsScheme && resolved.host() == base.host() && resolved.port() == base.port();
        if (filePath.isEmpty() || !resolved.isValid() || !isSameOrigin || !resolved.userInfo().isEmpty()) {
            return QUrl();
        }
        return resolved;
    }

    bool isNewerVersion(const QString &candidate, const QString &current)
    {
        qsizetype suffixIndex = 0;
        const QVersionNumber candidateVersion = QVersionNumber::fromString(candidate, &suffixIndex);
        if (candidateVersion.isNull() || suffixIndex != candidate.size()) {
            return false;
        }
        return QVersionNumber::compare(candidateVersion, QVersionNumber::fromString(current)) > 0;
    }

    ReplyDecision decideOnReply(int httpStatus, const QByteArray &body, const QString &backendUrl,
                                const QString &currentVersion)
    {
        ReplyDecision decision;

        if (httpStatus == httpUnauthorized || httpStatus == httpForbidden) {
            decision.action = ReplyAction::RejectKey;
            return decision;
        }
        if (httpStatus != httpOk) {
            return decision;
        }

        const QJsonDocument document = QJsonDocument::fromJson(body);
        if (!document.isObject() || !document.object().value(replyUpdateAvailableKey).isBool()) {
            return decision;
        }

        const QJsonObject reply = document.object();
        if (!reply.value(replyUpdateAvailableKey).toBool()) {
            decision.action = ReplyAction::NoUpdate;
            return decision;
        }

        decision.offer.version = reply.value(replyVersionKey).toString();
        decision.offer.fileUrl = fileUrl(backendUrl, reply.value(replyUrlKey).toString());
        decision.offer.sha256 = reply.value(replySha256Key).toString().toLower();
        decision.offer.releaseDate = reply.value(replyReleaseDateKey).toString();

        const QJsonObject changes = reply.value(replyChangesKey).toObject();
        decision.offer.newFeatures = changeLines(changes.value(changesNewKey));
        decision.offer.improvements = changeLines(changes.value(changesImprovedKey));
        decision.offer.bugFixes = changeLines(changes.value(changesFixedKey));

        if (!decision.offer.isValid()) {
            return decision;
        }

        decision.action =
                isNewerVersion(decision.offer.version, currentVersion) ? ReplyAction::Offer : ReplyAction::NoUpdate;
        return decision;
    }

    bool fileMatchesSha256(const QString &filePath, const QString &expectedSha256)
    {
        if (!isSha256Hex(expectedSha256)) {
            return false;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) {
            return false;
        }
        return hash.result().toHex() == expectedSha256.toLatin1();
    }

} // namespace amnezia::managedUpdate
