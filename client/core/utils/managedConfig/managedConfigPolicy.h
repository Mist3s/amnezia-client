#ifndef MANAGEDCONFIGPOLICY_H
#define MANAGEDCONFIGPOLICY_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

class QUrlQuery;

namespace amnezia::managedConfig
{

    constexpr int defaultPollIntervalHours = 6;
    constexpr int minPollIntervalHours = 1;
    constexpr int maxPollIntervalHours = 168;
    constexpr int connectTimeoutMs = 30 * 1000;

    enum class ReplyAction {
        Apply,
        KeepCurrent,
        RejectKey,
        TryNextBackend
    };

    struct RemoteServer
    {
        QString id;
        QString name;
        QString revision;
        QString config;
    };

    struct ReplyDecision
    {
        ReplyAction action = ReplyAction::TryNextBackend;
        QString revision;
        QString mainId;
        QList<RemoteServer> servers;
        bool isServerList = false;
        int pollIntervalHours = 0;
    };

    struct LocalServer
    {
        QString serverId;
        QString remoteId;
        QString revision;
    };

    struct KeptServer
    {
        QString serverId;
        RemoteServer remote;
        bool isConfigChanged = false;
    };

    struct ServerListChanges
    {
        QList<KeptServer> kept;
        QList<RemoteServer> added;
        QStringList removedServerIds;
        QString mainId;
        bool isMainChanged = false;
    };

    struct DeviceDescription
    {
        QString platform;
        QString version;
        QString abi;
        int sdk = 0;
    };

    struct SetupRequest
    {
        QStringList backendUrls;
        QString deviceKey;
        bool connect = false;
        bool autoStart = false;

        bool isValid() const;
    };

    QStringList parseBackendUrls(const QString &input);
    SetupRequest parseSetupRequest(const QByteArray &content);
    bool haveCommonBackend(const QStringList &left, const QStringList &right);
    QStringList orderedBackendUrls(const QStringList &backendUrls, const QString &lastWorkingUrl);
    QString androidAbi(const QString &cpuArchitecture);
    void describeDevice(QUrlQuery &query, const DeviceDescription &device);
    QUrl configEndpoint(const QString &backendUrl, const DeviceDescription &device);

    ReplyDecision decideOnReply(int httpStatus, const QByteArray &body, const QString &currentRevision);
    ServerListChanges planServerListChanges(const QList<LocalServer> &localServers, const QString &localMainId,
                                            const ReplyDecision &decision);

    int normalizedPollIntervalHours(int requestedHours);
    qint64 nextPollDelayMs(int intervalHours, double randomUnit);
    qint64 retryDelayMs(int failedAttempts);

} // namespace amnezia::managedConfig

#endif // MANAGEDCONFIGPOLICY_H
