#ifndef MANAGEDCONFIGCONTROLLER_H
#define MANAGEDCONFIGCONTROLLER_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include "core/controllers/connectionController.h"
#include "core/controllers/selfhosted/importController.h"
#include "core/protocols/vpnProtocol.h"
#include "core/repositories/managedConfigRepository.h"
#include "core/repositories/secureServersRepository.h"

class QNetworkAccessManager;
class QNetworkReply;

class ManagedConfigController : public QObject
{
    Q_OBJECT

public:
    enum class RefreshResult {
        Updated = 0,
        UpToDate,
        BackendUnreachable,
        KeyRejected,
        InvalidConfig
    };
    Q_ENUM(RefreshResult)

    explicit ManagedConfigController(SecureServersRepository *serversRepository,
                                     ManagedConfigRepository *managedConfigRepository, ImportController *importController,
                                     ConnectionController *connectionController, QObject *parent = nullptr);

    bool isSupported(const QString &serverId) const;
    bool isManaged(const QString &serverId) const;
    bool isRefreshing(const QString &serverId) const;
    ManagedConfigSettings settings(const QString &serverId) const;
    ManagedConfigSettings updateSource() const;

    void configure(const QString &serverId, const QStringList &backendUrls, const QString &deviceKey);
    void setup(const QStringList &backendUrls, const QString &deviceKey, bool connectAfterSetup);

    void start();
    void refresh(const QString &serverId);
    void refreshBeforeConnect(const QString &serverId);

public slots:
    void forget(const QString &serverId);

signals:
    void refreshStarted(const QString &serverId);
    void refreshFinished(const QString &serverId, ManagedConfigController::RefreshResult result);
    void readyToConnect();
    void setupStarted();
    void setupFinished(const QString &serverId, ManagedConfigController::RefreshResult result);

private:
    enum class Plan {
        Idle,
        ConnectAfterRefresh,
        ReconnectAfterClose,
        RecoverAfterClose,
        ReconnectAfterRecoveryRefresh,
        ReconnectAfterErrorRefresh
    };

    QNetworkAccessManager *networkManager() const;

    QNetworkReply *requestConfig(const QString &backendUrl, const QString &deviceKey, const QString &revision) const;
    QJsonObject nativeConfigFrom(const QString &config) const;

    void requestSetupConfig(const ManagedConfigSettings &settings, QStringList pendingBackendUrls, bool connectAfterSetup);
    void finishSetup(const QString &serverId, RefreshResult result, bool connectAfterSetup);
    void connectTo(const QString &serverId);

    void requestNextBackend(const QString &serverId);
    void handleBackendReply(const QString &serverId, const QString &backendUrl, QNetworkReply *reply);
    bool applyConfig(const QString &serverId, const QString &config);
    void finishRefresh(const QString &serverId, RefreshResult result);
    void abandonRefresh(const QString &serverId);
    void continuePlan(const QString &serverId, RefreshResult result);

    void pollIfDue();

    void onConnectionRequested(const QString &serverId);
    void onConnectionStateChanged(Vpn::ConnectionState state);
    bool isConnectedWithStaleConfig() const;
    void reconnectWithFreshConfig();
    void refreshToReconnect(Plan plan, const QString &serverId);
    void reopenConnection(const QString &serverId);

    void checkTunnel();
    void probeTunnel(const QStringList &probeUrls);
    void onTunnelProbed(bool isAlive);

    SecureServersRepository *m_serversRepository;
    ManagedConfigRepository *m_managedConfigRepository;
    ImportController *m_importController;
    ConnectionController *m_connectionController;

    QHash<QString, QStringList> m_pendingBackendUrls;

    QTimer m_pollTimer;
    qint64 m_nextPollAt = 0;

    Plan m_plan = Plan::Idle;
    QString m_planServerId;
    QTimer m_connectWaitTimer;

    Vpn::ConnectionState m_connectionState = Vpn::ConnectionState::Disconnected;
    QString m_connectionServerId;
    QString m_connectionRevision;

    QTimer m_tunnelCheckTimer;
    int m_failedTunnelChecks = 0;
    int m_tunnelCheckBackoff = 1;
};

#endif // MANAGEDCONFIGCONTROLLER_H
