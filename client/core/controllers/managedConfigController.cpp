#include "managedConfigController.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSysInfo>

#include "amneziaApplication.h"
#include "core/utils/managedConfig/managedConfigPolicy.h"
#include "core/utils/serverConfigUtils.h"
#include "logger.h"
#include "version.h"

namespace
{
    Logger logger("ManagedConfigController");

    constexpr int backendRequestTimeoutMs = 10 * 1000;
    constexpr int connectWaitTimeoutMs = 5 * 1000;
    constexpr int startupPollDelayMs = 5 * 1000;
    constexpr int pollCheckIntervalMs = 5 * 60 * 1000;
    constexpr int reopenConnectionDelayMs = 500;

    constexpr int tunnelCheckIntervalMs = 5 * 60 * 1000;
    constexpr int tunnelCheckRetryDelayMs = 30 * 1000;
    constexpr int tunnelProbeTimeoutMs = 7 * 1000;
    constexpr int failedTunnelChecksToRecover = 2;
    constexpr int maxTunnelCheckBackoff = 12;

    const QStringList tunnelProbeUrls = { "https://www.gstatic.com/generate_204",
                                          "https://cp.cloudflare.com/generate_204" };

    int httpStatusOf(QNetworkReply *reply)
    {
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }
}

ManagedConfigController::ManagedConfigController(SecureServersRepository *serversRepository,
                                                 ManagedConfigRepository *managedConfigRepository,
                                                 ImportController *importController,
                                                 ConnectionController *connectionController, QObject *parent)
    : QObject(parent),
      m_serversRepository(serversRepository),
      m_managedConfigRepository(managedConfigRepository),
      m_importController(importController),
      m_connectionController(connectionController)
{
    m_pollTimer.setInterval(pollCheckIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &ManagedConfigController::pollIfDue);

    m_connectWaitTimer.setSingleShot(true);
    m_connectWaitTimer.setInterval(connectWaitTimeoutMs);
    connect(&m_connectWaitTimer, &QTimer::timeout, this, [this]() {
        if (m_plan == Plan::ConnectAfterRefresh) {
            m_plan = Plan::Idle;
            emit readyToConnect();
        }
    });

    m_tunnelCheckTimer.setSingleShot(true);
    connect(&m_tunnelCheckTimer, &QTimer::timeout, this, &ManagedConfigController::checkTunnel);

    m_connectTimeoutTimer.setSingleShot(true);
    m_connectTimeoutTimer.setInterval(managedConfig::connectTimeoutMs);
    connect(&m_connectTimeoutTimer, &QTimer::timeout, this, &ManagedConfigController::giveUpConnecting);

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &ManagedConfigController::retryConnecting);

    connect(m_connectionController, &ConnectionController::openConnectionRequested, this,
            [this](const QString &serverId) { onConnectionRequested(serverId); });
#if defined(Q_OS_ANDROID)
    m_connectionController->setConfigurationExtender([this](const QString &serverId, QJsonObject &vpnConfiguration) {
        addServiceInfo(serverId, vpnConfiguration);
    });
#endif
    connect(m_connectionController, &ConnectionController::connectionStateChanged, this,
            &ManagedConfigController::onConnectionStateChanged);
}

managedConfig::DeviceDescription ManagedConfigController::thisDevice()
{
    managedConfig::DeviceDescription device;
    device.version = QString(APP_VERSION);
    device.abi = QSysInfo::buildCpuArchitecture();
#if defined(Q_OS_ANDROID)
    device.platform = "android";
    device.abi = managedConfig::androidAbi(device.abi);
    device.sdk = QNativeInterface::QAndroidApplication::sdkVersion();
#elif defined(Q_OS_WINDOWS)
    device.platform = "windows";
#elif defined(Q_OS_LINUX)
    device.platform = "linux";
#endif
    return device;
}

bool ManagedConfigController::isSupported(const QString &serverId) const
{
    return m_serversRepository->serverKind(serverId) == serverConfigUtils::ConfigType::Native;
}

bool ManagedConfigController::isManaged(const QString &serverId) const
{
    return isSupported(serverId) && m_managedConfigRepository->settings(serverId).isConfigured();
}

bool ManagedConfigController::isRefreshing(const QString &serverId) const
{
    return m_pendingBackendUrls.contains(serverId);
}

ManagedConfigSettings ManagedConfigController::settings(const QString &serverId) const
{
    return m_managedConfigRepository->settings(serverId);
}

ManagedConfigSettings ManagedConfigController::updateSource() const
{
    const QString defaultServerId = m_serversRepository->defaultServerId();
    if (isManaged(defaultServerId)) {
        return settings(defaultServerId);
    }
    for (const QString &serverId : m_managedConfigRepository->managedServerIds()) {
        if (isManaged(serverId)) {
            return settings(serverId);
        }
    }
    return ManagedConfigSettings();
}

void ManagedConfigController::configure(const QString &serverId, const QStringList &backendUrls, const QString &deviceKey)
{
    if (!isSupported(serverId)) {
        return;
    }

    const ManagedConfigSettings current = m_managedConfigRepository->settings(serverId);
    const QStringList group = current.isConfigured() ? serverGroup(current.deviceKey) : QStringList { serverId };
    if (backendUrls.isEmpty() && deviceKey.isEmpty()) {
        for (const QString &memberId : group) {
            forget(memberId);
        }
        return;
    }
    if (current.backendUrls == backendUrls && current.deviceKey == deviceKey) {
        return;
    }

    for (const QString &memberId : group) {
        const ManagedConfigSettings previous = m_managedConfigRepository->settings(memberId);
        ManagedConfigSettings fresh;
        fresh.backendUrls = backendUrls;
        fresh.deviceKey = deviceKey;
        fresh.mainId = previous.mainId;
        fresh.remoteId = previous.remoteId;
        fresh.remoteName = previous.remoteName;
        abandonRefresh(memberId);
        m_managedConfigRepository->setSettings(memberId, fresh);
    }
    m_nextPollAt = 0;
}

void ManagedConfigController::forget(const QString &serverId)
{
    const QString deviceKey = m_managedConfigRepository->settings(serverId).deviceKey;
    abandonRefresh(serverId);
    if (serverId == m_connectionServerId) {
        m_retryTimer.stop();
    }
    m_managedConfigRepository->remove(serverId);

    for (const QString &memberId : serverGroup(deviceKey)) {
        ManagedConfigSettings settings = m_managedConfigRepository->settings(memberId);
        if (!settings.listRevision.isEmpty()) {
            settings.listRevision.clear();
            m_managedConfigRepository->setSettings(memberId, settings);
        }
    }
}

QStringList ManagedConfigController::serverGroup(const QString &deviceKey) const
{
    QStringList group;
    for (const QString &serverId : m_managedConfigRepository->managedServerIds()) {
        if (isSupported(serverId) && m_managedConfigRepository->settings(serverId).deviceKey == deviceKey) {
            group.append(serverId);
        }
    }
    return group;
}

QString ManagedConfigController::mainServerOf(const QStringList &group) const
{
    for (const QString &serverId : group) {
        const ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
        if (settings.remoteId == settings.mainId) {
            return serverId;
        }
    }
    return group.value(0);
}

void ManagedConfigController::abandonRefresh(const QString &serverId)
{
    m_pendingBackendUrls.remove(serverId);
    if (serverId == m_planServerId
        && (m_plan == Plan::ReconnectAfterRecoveryRefresh || m_plan == Plan::ReconnectAfterErrorRefresh)) {
        m_plan = Plan::Idle;
    }
}

void ManagedConfigController::start()
{
    if (auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(application, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
                pollIfDue();
            }
        });
    }

    m_pollTimer.start();
    QTimer::singleShot(startupPollDelayMs, this, &ManagedConfigController::pollIfDue);
}

void ManagedConfigController::refresh(const QString &serverId)
{
    if (!isManaged(serverId) || isRefreshing(serverId)) {
        return;
    }

    const ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
    m_pendingBackendUrls.insert(serverId,
                                managedConfig::orderedBackendUrls(settings.backendUrls, settings.lastWorkingUrl));
    emit refreshStarted(serverId);
    requestNextBackend(serverId);
}

void ManagedConfigController::refreshBeforeConnect(const QString &serverId)
{
    m_plan = Plan::ConnectAfterRefresh;
    m_planServerId = serverId;
    m_connectWaitTimer.start();
    refresh(serverId);
}

QNetworkAccessManager *ManagedConfigController::networkManager() const
{
    auto *application = qobject_cast<AmneziaApplication *>(QCoreApplication::instance());
    return application ? application->networkManager() : nullptr;
}

QNetworkReply *ManagedConfigController::requestConfig(const QString &backendUrl, const QString &deviceKey,
                                                      const QString &revision) const
{
    QNetworkAccessManager *manager = networkManager();
    if (!manager || backendUrl.isEmpty()) {
        return nullptr;
    }

    QNetworkRequest request(managedConfig::configEndpoint(backendUrl, thisDevice()));
    request.setTransferTimeout(backendRequestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + deviceKey.toUtf8());
    if (!revision.isEmpty()) {
        request.setRawHeader("If-None-Match", '"' + revision.toUtf8() + '"');
    }
    return manager->get(request);
}

void ManagedConfigController::setup(const QStringList &backendUrls, const QString &deviceKey, bool connectAfterSetup)
{
    emit setupStarted();

    for (const QString &serverId : m_managedConfigRepository->managedServerIds()) {
        if (!isManaged(serverId)) {
            continue;
        }
        const ManagedConfigSettings current = settings(serverId);
        if (current.deviceKey == deviceKey || managedConfig::haveCommonBackend(current.backendUrls, backendUrls)) {
            const QString mainServerId = mainServerOf(serverGroup(current.deviceKey));
            configure(mainServerId, backendUrls, deviceKey);
            refresh(mainServerId);
            finishSetup(mainServerId, RefreshResult::UpToDate, connectAfterSetup);
            return;
        }
    }

    ManagedConfigSettings fresh;
    fresh.backendUrls = backendUrls;
    fresh.deviceKey = deviceKey;
    requestSetupConfig(fresh, backendUrls, connectAfterSetup);
}

void ManagedConfigController::requestSetupConfig(const ManagedConfigSettings &settings, QStringList pendingBackendUrls,
                                                 bool connectAfterSetup)
{
    const QString backendUrl = pendingBackendUrls.isEmpty() ? QString() : pendingBackendUrls.takeFirst();
    QNetworkReply *reply = requestConfig(backendUrl, settings.deviceKey, QString());
    if (!reply) {
        finishSetup(QString(), RefreshResult::BackendUnreachable, false);
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [this, settings, pendingBackendUrls, connectAfterSetup, backendUrl, reply]() {
        reply->deleteLater();
        const managedConfig::ReplyDecision decision =
                managedConfig::decideOnReply(httpStatusOf(reply), reply->readAll(), QString());

        switch (decision.action) {
        case managedConfig::ReplyAction::TryNextBackend: {
            requestSetupConfig(settings, pendingBackendUrls, connectAfterSetup);
            return;
        }
        case managedConfig::ReplyAction::RejectKey: {
            finishSetup(QString(), RefreshResult::KeyRejected, false);
            return;
        }
        case managedConfig::ReplyAction::KeepCurrent: {
            finishSetup(QString(), RefreshResult::InvalidConfig, false);
            return;
        }
        case managedConfig::ReplyAction::Apply: {
            const ListOutcome outcome = applyServerList(settings, backendUrl, decision);
            finishSetup(outcome.mainServerId,
                        outcome.mainServerId.isEmpty() ? RefreshResult::InvalidConfig : RefreshResult::Updated,
                        connectAfterSetup && !outcome.mainServerId.isEmpty());
            return;
        }
        }
    });
}

void ManagedConfigController::finishSetup(const QString &serverId, RefreshResult result, bool connectAfterSetup)
{
    logger.info() << "Managed config setup finished with result" << static_cast<int>(result);

    if (!serverId.isEmpty()) {
        m_serversRepository->setDefaultServer(serverId);
        if (connectAfterSetup) {
            connectTo(serverId);
        }
    }
    emit setupFinished(serverId, result);
}

void ManagedConfigController::connectTo(const QString &serverId)
{
    if (m_connectionState != Vpn::ConnectionState::Disconnected && m_connectionServerId == serverId) {
        return;
    }

    if (m_connectionState == Vpn::ConnectionState::Disconnected) {
        reopenConnection(serverId);
        return;
    }
    m_plan = Plan::ReconnectAfterClose;
    m_planServerId = serverId;
    m_connectionController->closeConnection();
}

void ManagedConfigController::requestNextBackend(const QString &serverId)
{
    const QString backendUrl =
            m_pendingBackendUrls.value(serverId).isEmpty() ? QString() : m_pendingBackendUrls[serverId].takeFirst();
    const ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);

    QNetworkReply *reply = requestConfig(backendUrl, settings.deviceKey, settings.listRevision);
    if (!reply) {
        finishRefresh(serverId, RefreshResult::BackendUnreachable);
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [this, serverId, backendUrl, reply]() {
        reply->deleteLater();
        handleBackendReply(serverId, backendUrl, reply);
    });
}

void ManagedConfigController::handleBackendReply(const QString &serverId, const QString &backendUrl, QNetworkReply *reply)
{
    if (!isRefreshing(serverId)) {
        return;
    }

    const ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
    const managedConfig::ReplyDecision decision =
            managedConfig::decideOnReply(httpStatusOf(reply), reply->readAll(), settings.listRevision);

    switch (decision.action) {
    case managedConfig::ReplyAction::TryNextBackend: {
        requestNextBackend(serverId);
        return;
    }
    case managedConfig::ReplyAction::RejectKey: {
        finishRefresh(serverId, RefreshResult::KeyRejected);
        return;
    }
    case managedConfig::ReplyAction::KeepCurrent: {
        for (const QString &memberId : serverGroup(settings.deviceKey)) {
            ManagedConfigSettings member = m_managedConfigRepository->settings(memberId);
            member.lastWorkingUrl = backendUrl;
            if (decision.pollIntervalHours > 0) {
                member.pollIntervalHours = decision.pollIntervalHours;
            }
            m_managedConfigRepository->setSettings(memberId, member);
        }
        finishRefresh(serverId, RefreshResult::UpToDate);
        return;
    }
    case managedConfig::ReplyAction::Apply: {
        const ListOutcome outcome = applyServerList(settings, backendUrl, decision);
        finishRefresh(isManaged(serverId) ? serverId : outcome.mainServerId, outcome.result);
        return;
    }
    }
}

ManagedConfigController::ListOutcome ManagedConfigController::applyServerList(const ManagedConfigSettings &source,
                                                                              const QString &backendUrl,
                                                                              const managedConfig::ReplyDecision &decision)
{
    const QStringList group = serverGroup(source.deviceKey);
    QList<managedConfig::LocalServer> localServers;
    for (const QString &serverId : group) {
        const ManagedConfigSettings member = m_managedConfigRepository->settings(serverId);
        localServers.append({ serverId, member.remoteId, member.revision });
    }
    const managedConfig::ServerListChanges changes =
            managedConfig::planServerListChanges(localServers, source.mainId, decision);

    QHash<QString, QJsonObject> configs;
    QList<managedConfig::RemoteServer> changedServers = changes.added;
    for (const managedConfig::KeptServer &kept : changes.kept) {
        if (kept.isConfigChanged) {
            changedServers.append(kept.remote);
        }
    }
    for (const managedConfig::RemoteServer &remote : changedServers) {
        const QJsonObject config = nativeConfigFrom(remote.config);
        if (config.isEmpty()) {
            return ListOutcome();
        }
        configs.insert(remote.id, config);
    }

    bool isChanged = changes.isMainChanged || !changes.added.isEmpty() || !changes.removedServerIds.isEmpty();
    QList<QPair<QString, managedConfig::RemoteServer>> members;
    for (const managedConfig::KeptServer &kept : changes.kept) {
        const QString previousName = m_managedConfigRepository->settings(kept.serverId).remoteName;
        isChanged |= updateServer(kept.serverId, configs.value(kept.remote.id), kept.remote.name, previousName);
        members.append({ kept.serverId, kept.remote });
    }
    for (const managedConfig::RemoteServer &remote : changes.added) {
        const QString serverId = m_serversRepository->addServer(QString(), configs.value(remote.id),
                                                                serverConfigUtils::ConfigType::Native);
        if (isSupported(serverId)) {
            updateServer(serverId, QJsonObject(), remote.name, QString());
            members.append({ serverId, remote });
        }
    }

    ListOutcome outcome;
    for (const auto &member : members) {
        if (member.second.id == changes.mainId) {
            outcome.mainServerId = member.first;
        }
    }
    if (outcome.mainServerId.isEmpty()) {
        return outcome;
    }
    outcome.result = isChanged ? RefreshResult::Updated : RefreshResult::UpToDate;

    const QStringList &removed = changes.removedServerIds;
    const QString connectedServerId = m_connectionServerId;
    const bool isPlanServerRemoved = m_plan != Plan::Idle && removed.contains(m_planServerId);
    if (isPlanServerRemoved) {
        m_planServerId = outcome.mainServerId;
    }
    if (changes.isMainChanged || isPlanServerRemoved || removed.contains(connectedServerId)
        || removed.contains(m_serversRepository->defaultServerId())) {
        m_serversRepository->setDefaultServer(outcome.mainServerId);
    }
    for (const QString &serverId : removed) {
        m_serversRepository->removeServer(serverId);
    }

    for (const auto &member : members) {
        ManagedConfigSettings fresh = m_managedConfigRepository->settings(member.first);
        fresh.backendUrls = source.backendUrls;
        fresh.deviceKey = source.deviceKey;
        fresh.revision = member.second.revision;
        fresh.listRevision = decision.revision;
        fresh.mainId = changes.mainId;
        fresh.remoteId = member.second.id;
        fresh.remoteName = member.second.name;
        fresh.lastWorkingUrl = backendUrl;
        fresh.pollIntervalHours = decision.pollIntervalHours;
        fresh.lastCheckedAt = QDateTime::currentSecsSinceEpoch();
        fresh.lastResult = static_cast<int>(outcome.result);
        m_managedConfigRepository->setSettings(member.first, fresh);
    }

    const bool isOnLeftServer = removed.contains(connectedServerId)
            || (changes.isMainChanged && group.contains(connectedServerId) && connectedServerId != outcome.mainServerId);
    if (m_plan == Plan::Idle && m_connectionState != Vpn::ConnectionState::Disconnected && isOnLeftServer) {
        logger.info() << "The backend moved this device to another server, reconnecting";
        connectTo(outcome.mainServerId);
    }
    return outcome;
}

bool ManagedConfigController::updateServer(const QString &serverId, const QJsonObject &config, const QString &name,
                                           const QString &previousName)
{
    const auto current = m_serversRepository->nativeConfig(serverId);
    if (!current.has_value()) {
        return false;
    }

    const bool isNamedByBackend = previousName.isEmpty() || current->description == previousName;
    const bool isRenamed = !name.isEmpty() && name != current->description && isNamedByBackend;
    if (config.isEmpty() && !isRenamed) {
        return false;
    }

    NativeServerConfig fresh = config.isEmpty() ? *current : NativeServerConfig::fromJson(config);
    fresh.description = isRenamed ? name : current->description;
    fresh.displayName = isRenamed ? name : current->displayName;
    m_serversRepository->editServer(serverId, fresh.toJson(), serverConfigUtils::ConfigType::Native);
    return true;
}

QJsonObject ManagedConfigController::nativeConfigFrom(const QString &config) const
{
    const ImportController::ImportResult imported = m_importController->extractConfigFromData(config);
    if (imported.errorCode != ErrorCode::NoError
        || serverConfigUtils::configTypeFromJson(imported.config) != serverConfigUtils::ConfigType::Native) {
        return QJsonObject();
    }
    return imported.config;
}

void ManagedConfigController::finishRefresh(const QString &serverId, RefreshResult result)
{
    m_pendingBackendUrls.remove(serverId);

    for (const QString &memberId : serverGroup(m_managedConfigRepository->settings(serverId).deviceKey)) {
        ManagedConfigSettings settings = m_managedConfigRepository->settings(memberId);
        settings.lastCheckedAt = QDateTime::currentSecsSinceEpoch();
        settings.lastResult = static_cast<int>(result);
        m_managedConfigRepository->setSettings(memberId, settings);
    }

    logger.info() << "Managed config refresh finished with result" << static_cast<int>(result);

    continuePlan(serverId, result);
    emit refreshFinished(serverId, result);
}

void ManagedConfigController::continuePlan(const QString &serverId, RefreshResult result)
{
    if (serverId == m_planServerId) {
        switch (m_plan) {
        case Plan::ConnectAfterRefresh: {
            m_connectWaitTimer.stop();
            m_plan = Plan::Idle;
            emit readyToConnect();
            return;
        }
        case Plan::ReconnectAfterRecoveryRefresh: {
            m_plan = Plan::Idle;
            m_tunnelCheckBackoff =
                    result == RefreshResult::Updated ? 1 : qMin(m_tunnelCheckBackoff * 2, maxTunnelCheckBackoff);
            reopenConnection(serverId);
            return;
        }
        case Plan::ReconnectAfterErrorRefresh: {
            m_plan = Plan::Idle;
            if (result == RefreshResult::Updated) {
                reopenConnection(serverId);
            }
            return;
        }
        case Plan::ReconnectAfterRetryRefresh: {
            m_plan = Plan::Idle;
            reopenConnection(serverId);
            return;
        }
        default: break;
        }
    }

    if (isConnectedWithStaleConfig()) {
        reconnectWithFreshConfig();
    }
}

void ManagedConfigController::pollIfDue()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now < m_nextPollAt) {
        return;
    }

    const QStringList serverIds = m_managedConfigRepository->managedServerIds();
    if (serverIds.isEmpty()) {
        return;
    }

    int intervalHours = managedConfig::maxPollIntervalHours;
    QStringList polledDeviceKeys;
    for (const QString &serverId : serverIds) {
        const ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
        intervalHours = qMin(intervalHours, managedConfig::normalizedPollIntervalHours(settings.pollIntervalHours));
        if (isManaged(serverId) && !polledDeviceKeys.contains(settings.deviceKey)) {
            polledDeviceKeys.append(settings.deviceKey);
            refresh(serverId);
        }
    }

    m_nextPollAt =
            now + managedConfig::nextPollDelayMs(intervalHours, QRandomGenerator::global()->generateDouble()) / 1000;
}

void ManagedConfigController::addServiceInfo(const QString &serverId, QJsonObject &vpnConfiguration) const
{
    if (!isManaged(serverId)) {
        return;
    }
    const ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
    const managedConfig::ServiceSource source { settings.backendUrls, settings.lastWorkingUrl, settings.deviceKey,
                                                settings.remoteId,    settings.mainId,         settings.revision,
                                                settings.listRevision };
    vpnConfiguration.insert(managedConfig::serviceInfoKey, managedConfig::serviceInfo(source, thisDevice()));
}

void ManagedConfigController::onConnectionRequested(const QString &serverId)
{
    m_retryTimer.stop();
    m_connectionServerId = serverId;
    m_connectionRevision = m_managedConfigRepository->settings(serverId).revision;
}

void ManagedConfigController::onConnectionStateChanged(Vpn::ConnectionState state)
{
    if (state == m_connectionState) {
        return;
    }
    m_connectionState = state;
    m_tunnelCheckTimer.stop();
    m_connectTimeoutTimer.stop();

    switch (state) {
    case Vpn::ConnectionState::Connecting:
    case Vpn::ConnectionState::Reconnecting: {
#if !defined(Q_OS_ANDROID)
        if (isManaged(m_connectionServerId)) {
            m_connectTimeoutTimer.start();
        }
#endif
        return;
    }
    case Vpn::ConnectionState::Connected: {
        if (m_connectionServerId.isEmpty()) {
            onConnectionRequested(m_serversRepository->defaultServerId());
        }
        m_failedConnects = 0;
        if (!isManaged(m_connectionServerId)) {
            return;
        }
        if (isConnectedWithStaleConfig()) {
            reconnectWithFreshConfig();
            return;
        }
        m_failedTunnelChecks = 0;
        m_tunnelCheckTimer.start(tunnelCheckIntervalMs * m_tunnelCheckBackoff);
        return;
    }
    case Vpn::ConnectionState::Disconnected: {
        const QString serverId = m_planServerId;
        if (m_plan == Plan::ReconnectAfterClose) {
            m_plan = Plan::Idle;
            reopenConnection(serverId);
        } else if (m_plan == Plan::RecoverAfterClose) {
            refreshToReconnect(Plan::ReconnectAfterRecoveryRefresh, serverId);
        } else if (m_plan == Plan::GiveUpAfterClose) {
            m_retryTimer.start(managedConfig::retryDelayMs(m_failedConnects));
            refreshToReconnect(Plan::ReconnectAfterErrorRefresh, serverId);
        } else if (m_plan == Plan::Idle) {
            m_connectionServerId.clear();
            m_retryTimer.stop();
            m_failedConnects = 0;
        }
        return;
    }
    case Vpn::ConnectionState::Error: {
        const bool isOwnAttempt = m_plan != Plan::Idle;
        m_plan = Plan::Idle;
        if (!isOwnAttempt) {
            refreshToReconnect(Plan::ReconnectAfterErrorRefresh, m_connectionServerId);
        }
        return;
    }
    default: return;
    }
}

void ManagedConfigController::refreshToReconnect(Plan plan, const QString &serverId)
{
    if (!isManaged(serverId)) {
        m_plan = Plan::Idle;
        return;
    }
    m_plan = plan;
    m_planServerId = serverId;
    refresh(serverId);
}

bool ManagedConfigController::isConnectedWithStaleConfig() const
{
    return m_plan == Plan::Idle && m_connectionState == Vpn::ConnectionState::Connected
            && isManaged(m_connectionServerId)
            && m_managedConfigRepository->settings(m_connectionServerId).revision != m_connectionRevision;
}

void ManagedConfigController::reconnectWithFreshConfig()
{
    m_plan = Plan::ReconnectAfterClose;
    m_planServerId = m_connectionServerId;
    m_connectionController->closeConnection();
}

void ManagedConfigController::reopenConnection(const QString &serverId)
{
    QTimer::singleShot(reopenConnectionDelayMs, this, [this, serverId]() {
        const QString targetServerId = reconnectTarget(serverId);
        if (m_plan != Plan::Idle || m_connectionState != Vpn::ConnectionState::Disconnected || targetServerId.isEmpty()) {
            return;
        }
        const ErrorCode errorCode = m_connectionController->openConnection(targetServerId);
        if (errorCode != ErrorCode::NoError) {
            logger.error() << "Failed to reopen connection, error code" << static_cast<int>(errorCode);
        }
    });
}

QString ManagedConfigController::reconnectTarget(const QString &serverId) const
{
    const QString defaultServerId = m_serversRepository->defaultServerId();
    if (serverId == defaultServerId) {
        return serverId;
    }
    const bool isSameGroup = isManaged(serverId) && isManaged(defaultServerId)
            && settings(serverId).deviceKey == settings(defaultServerId).deviceKey;
    return isSameGroup ? defaultServerId : QString();
}

void ManagedConfigController::checkTunnel()
{
    if (m_plan != Plan::Idle || m_connectionState != Vpn::ConnectionState::Connected) {
        return;
    }
    probeTunnel(tunnelProbeUrls);
}

void ManagedConfigController::probeTunnel(const QStringList &probeUrls)
{
    QNetworkAccessManager *manager = networkManager();
    if (!manager || probeUrls.isEmpty()) {
        onTunnelProbed(false);
        return;
    }

    QNetworkRequest request { QUrl(probeUrls.first()) };
    request.setTransferTimeout(tunnelProbeTimeoutMs);

    QNetworkReply *reply = manager->head(request);
    connect(reply, &QNetworkReply::finished, this, [this, probeUrls, reply]() {
        reply->deleteLater();
        if (httpStatusOf(reply) > 0) {
            onTunnelProbed(true);
        } else {
            probeTunnel(probeUrls.mid(1));
        }
    });
}

void ManagedConfigController::giveUpConnecting()
{
    const bool isConnecting = m_connectionState == Vpn::ConnectionState::Connecting
            || m_connectionState == Vpn::ConnectionState::Reconnecting;
    if (!isConnecting || m_plan != Plan::Idle || !isManaged(m_connectionServerId)) {
        return;
    }

    ++m_failedConnects;
    logger.warning() << "The VPN server does not answer, using the plain internet until retry" << m_failedConnects;
    m_plan = Plan::GiveUpAfterClose;
    m_planServerId = m_connectionServerId;
    emit serverNotResponding();
    m_connectionController->closeConnection();
}

void ManagedConfigController::retryConnecting()
{
    if (m_plan != Plan::Idle || m_connectionState != Vpn::ConnectionState::Disconnected) {
        return;
    }
    refreshToReconnect(Plan::ReconnectAfterRetryRefresh, m_planServerId);
}

void ManagedConfigController::onTunnelProbed(bool isAlive)
{
    if (m_plan != Plan::Idle || m_connectionState != Vpn::ConnectionState::Connected) {
        return;
    }

    if (isAlive) {
        m_failedTunnelChecks = 0;
        m_tunnelCheckBackoff = 1;
        m_tunnelCheckTimer.start(tunnelCheckIntervalMs);
        return;
    }

    ++m_failedTunnelChecks;
    if (m_failedTunnelChecks < failedTunnelChecksToRecover) {
        m_tunnelCheckTimer.start(tunnelCheckRetryDelayMs);
        return;
    }

    logger.warning() << "Tunnel passes no traffic, asking the backend for a fresh config without VPN";
    m_failedTunnelChecks = 0;
    m_plan = Plan::RecoverAfterClose;
    m_planServerId = m_connectionServerId;
    m_connectionController->closeConnection();
}
