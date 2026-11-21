#include "managedConfigPolicy.h"

#include <algorithm>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>

namespace
{
    constexpr QLatin1String httpsScheme("https");
    constexpr QLatin1String configPath("/v1/config");

    constexpr QLatin1String replyRevisionKey("revision");
    constexpr QLatin1String replyConfigKey("config");
    constexpr QLatin1String replyPollIntervalHoursKey("poll_interval_hours");
    constexpr QLatin1String replyServerNameKey("server_name");
    constexpr QLatin1String replyServersKey("servers");
    constexpr QLatin1String replyMainKey("main");
    constexpr QLatin1String replyIdKey("id");
    constexpr QLatin1String replyNameKey("name");
    constexpr QLatin1String serverListQuery("servers=1");
    constexpr QLatin1String devicePlatformKey("platform");
    constexpr QLatin1String deviceVersionKey("version");
    constexpr QLatin1String deviceAbiKey("abi");
    constexpr QLatin1String deviceSdkKey("sdk");

    constexpr QLatin1String setupBackendKey("backend");
    constexpr QLatin1String setupDeviceKeyKey("key");
    constexpr QLatin1String setupConnectKey("connect");
    constexpr QLatin1String setupAutoStartKey("autostart");
    constexpr QByteArrayView utf8Bom("\xEF\xBB\xBF");

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

    amnezia::managedConfig::RemoteServer remoteServerFrom(const QJsonObject &entry, const QString &name)
    {
        amnezia::managedConfig::RemoteServer server;
        server.id = entry.value(replyIdKey).toString();
        server.name = name.simplified();
        server.revision = entry.value(replyRevisionKey).toString();
        server.config = entry.value(replyConfigKey).toString();
        return server;
    }

    bool isUsable(const amnezia::managedConfig::RemoteServer &server)
    {
        return !server.revision.isEmpty() && !server.config.trimmed().isEmpty();
    }

    QList<amnezia::managedConfig::RemoteServer> remoteServersFrom(const QJsonArray &entries)
    {
        QList<amnezia::managedConfig::RemoteServer> servers;
        QStringList ids;
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            const amnezia::managedConfig::RemoteServer server =
                    remoteServerFrom(entry, entry.value(replyNameKey).toString());
            if (server.id.isEmpty() || ids.contains(server.id) || !isUsable(server)) {
                return {};
            }
            ids.append(server.id);
            servers.append(server);
        }
        return servers;
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

    bool SetupRequest::isValid() const
    {
        return !backendUrls.isEmpty() && !deviceKey.isEmpty();
    }

    SetupRequest parseSetupRequest(const QByteArray &content)
    {
        const QByteArray json = content.startsWith(utf8Bom) ? content.mid(utf8Bom.size()) : content;
        const QJsonObject object = QJsonDocument::fromJson(json).object();

        SetupRequest request;
        request.backendUrls = parseBackendUrls(object.value(setupBackendKey).toString());
        request.deviceKey = object.value(setupDeviceKeyKey).toString().trimmed();
        request.connect = object.value(setupConnectKey).toBool();
        request.autoStart = object.value(setupAutoStartKey).toBool();
        return request;
    }

    bool haveCommonBackend(const QStringList &left, const QStringList &right)
    {
        return std::any_of(left.cbegin(), left.cend(), [&right](const QString &url) { return right.contains(url); });
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

    QString androidAbi(const QString &cpuArchitecture)
    {
        static const QHash<QString, QString> abis = {
            { "arm64", "arm64-v8a" }, { "arm", "armeabi-v7a" }, { "x86_64", "x86_64" }, { "i386", "x86" }
        };
        return abis.value(cpuArchitecture);
    }

    void describeDevice(QUrlQuery &query, const DeviceDescription &device)
    {
        const QList<std::pair<QString, QString>> items = {
            { devicePlatformKey, device.platform },
            { deviceVersionKey, device.version },
            { deviceAbiKey, device.abi },
            { deviceSdkKey, device.sdk > 0 ? QString::number(device.sdk) : QString() },
        };
        for (const auto &[key, value] : items) {
            if (!value.isEmpty()) {
                query.addQueryItem(key, value);
            }
        }
    }

    QUrl configEndpoint(const QString &backendUrl, const DeviceDescription &device)
    {
        QUrlQuery query(serverListQuery);
        describeDevice(query, device);
        QUrl endpoint(backendUrl + configPath);
        endpoint.setQuery(query);
        return endpoint;
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
        decision.pollIntervalHours = reply.value(replyPollIntervalHoursKey).toInt();
        decision.isServerList = reply.contains(replyServersKey);
        if (decision.isServerList) {
            decision.mainId = reply.value(replyMainKey).toString();
            decision.servers = remoteServersFrom(reply.value(replyServersKey).toArray());
        } else {
            const RemoteServer server = remoteServerFrom(reply, reply.value(replyServerNameKey).toString());
            if (isUsable(server)) {
                decision.servers.append(server);
            }
        }

        const bool hasMain =
                std::any_of(decision.servers.cbegin(), decision.servers.cend(),
                            [&decision](const RemoteServer &server) { return server.id == decision.mainId; });
        if (decision.revision.isEmpty() || !hasMain) {
            return decision;
        }

        decision.action = decision.revision == currentRevision ? ReplyAction::KeepCurrent : ReplyAction::Apply;
        return decision;
    }

    ServerListChanges planServerListChanges(const QList<LocalServer> &localServers, const QString &localMainId,
                                            const ReplyDecision &decision)
    {
        ServerListChanges changes;
        changes.mainId = decision.isServerList ? decision.mainId : localMainId;
        changes.isMainChanged = decision.isServerList && !localMainId.isEmpty() && localMainId != decision.mainId;

        QStringList keptServerIds;
        const auto localServerFor = [&](const QString &remoteId) {
            const auto byRemoteId =
                    std::find_if(localServers.cbegin(), localServers.cend(), [&](const LocalServer &server) {
                        return server.remoteId == remoteId && !keptServerIds.contains(server.serverId);
                    });
            if (byRemoteId != localServers.cend() || remoteId != changes.mainId) {
                return byRemoteId;
            }
            return std::find_if(localServers.cbegin(), localServers.cend(), [&](const LocalServer &server) {
                return server.remoteId.isEmpty() && !keptServerIds.contains(server.serverId);
            });
        };

        for (RemoteServer remote : decision.servers) {
            if (!decision.isServerList) {
                remote.id = changes.mainId;
            }
            const auto local = localServerFor(remote.id);
            if (local == localServers.cend()) {
                changes.added.append(remote);
                continue;
            }
            keptServerIds.append(local->serverId);
            changes.kept.append({ local->serverId, remote, local->revision != remote.revision });
        }

        for (const LocalServer &local : localServers) {
            if (decision.isServerList && !local.remoteId.isEmpty() && !keptServerIds.contains(local.serverId)) {
                changes.removedServerIds.append(local.serverId);
            }
        }
        return changes;
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
