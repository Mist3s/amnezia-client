#include "managedConfigController.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>

#include "amneziaApplication.h"
#include "core/utils/managedConfig/managedConfigPolicy.h"
#include "core/utils/serverConfigUtils.h"
#include "logger.h"

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

    connect(m_connectionController, &ConnectionController::openConnectionRequested, this,
            [this](const QString &serverId) { onConnectionRequested(serverId); });
    connect(m_connectionController, &ConnectionController::connectionStateChanged, this,
            &ManagedConfigController::onConnectionStateChanged);
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
    if (backendUrls.isEmpty() && deviceKey.isEmpty()) {
        forget(serverId);
        return;
    }

    ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
    if (settings.backendUrls == backendUrls && settings.deviceKey == deviceKey) {
        return;
    }

    ManagedConfigSettings fresh;
    fresh.backendUrls = backendUrls;
    fresh.deviceKey = deviceKey;
    abandonRefresh(serverId);
    m_managedConfigRepository->setSettings(serverId, fresh);
    m_nextPollAt = 0;
}

void ManagedConfigController::forget(const QString &serverId)
{
    abandonRefresh(serverId);
    m_managedConfigRepository->remove(serverId);
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

    QNetworkRequest request(managedConfig::configEndpoint(backendUrl));
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
            configure(serverId, backendUrls, deviceKey);
            refresh(serverId);
            finishSetup(serverId, RefreshResult::UpToDate, connectAfterSetup);
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
            const QJsonObject config = nativeConfigFrom(decision.config);
            const QString serverId = config.isEmpty()
                    ? QString()
                    : m_serversRepository->addServer(QString(), config, serverConfigUtils::ConfigType::Native);
            if (!isSupported(serverId)) {
                finishSetup(QString(), RefreshResult::InvalidConfig, false);
                return;
            }

            ManagedConfigSettings fresh = settings;
            fresh.lastWorkingUrl = backendUrl;
            fresh.revision = decision.revision;
            fresh.pollIntervalHours = decision.pollIntervalHours;
            fresh.lastCheckedAt = QDateTime::currentSecsSinceEpoch();
            fresh.lastResult = static_cast<int>(RefreshResult::Updated);
            m_managedConfigRepository->setSettings(serverId, fresh);
            finishSetup(serverId, RefreshResult::Updated, connectAfterSetup);
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

    QNetworkReply *reply = requestConfig(backendUrl, settings.deviceKey, settings.revision);
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

    ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
    const managedConfig::ReplyDecision decision =
            managedConfig::decideOnReply(httpStatusOf(reply), reply->readAll(), settings.revision);

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
        settings.lastWorkingUrl = backendUrl;
        if (decision.pollIntervalHours > 0) {
            settings.pollIntervalHours = decision.pollIntervalHours;
        }
        m_managedConfigRepository->setSettings(serverId, settings);
        finishRefresh(serverId, RefreshResult::UpToDate);
        return;
    }
    case managedConfig::ReplyAction::Apply: {
        if (!applyConfig(serverId, decision.config)) {
            finishRefresh(serverId, RefreshResult::InvalidConfig);
            return;
        }
        settings.lastWorkingUrl = backendUrl;
        settings.revision = decision.revision;
        settings.pollIntervalHours = decision.pollIntervalHours;
        m_managedConfigRepository->setSettings(serverId, settings);
        finishRefresh(serverId, RefreshResult::Updated);
        return;
    }
    }
}

bool ManagedConfigController::applyConfig(const QString &serverId, const QString &config)
{
    const auto current = m_serversRepository->nativeConfig(serverId);
    if (!current.has_value()) {
        return false;
    }

    const QJsonObject imported = nativeConfigFrom(config);
    if (imported.isEmpty()) {
        return false;
    }

    NativeServerConfig fresh = NativeServerConfig::fromJson(imported);
    fresh.description = current->description;
    fresh.displayName = current->displayName;
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

    ManagedConfigSettings settings = m_managedConfigRepository->settings(serverId);
    settings.lastCheckedAt = QDateTime::currentSecsSinceEpoch();
    settings.lastResult = static_cast<int>(result);
    m_managedConfigRepository->setSettings(serverId, settings);

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
    for (const QString &serverId : serverIds) {
        const int serverIntervalHours = m_managedConfigRepository->settings(serverId).pollIntervalHours;
        intervalHours = qMin(intervalHours, managedConfig::normalizedPollIntervalHours(serverIntervalHours));
        refresh(serverId);
    }

    m_nextPollAt =
            now + managedConfig::nextPollDelayMs(intervalHours, QRandomGenerator::global()->generateDouble()) / 1000;
}

void ManagedConfigController::onConnectionRequested(const QString &serverId)
{
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

    switch (state) {
    case Vpn::ConnectionState::Connected: {
        if (m_connectionServerId.isEmpty()) {
            onConnectionRequested(m_serversRepository->defaultServerId());
        }
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
        } else if (m_plan == Plan::Idle) {
            m_connectionServerId.clear();
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
        if (m_plan != Plan::Idle || m_connectionState != Vpn::ConnectionState::Disconnected
            || serverId != m_serversRepository->defaultServerId()) {
            return;
        }
        const ErrorCode errorCode = m_connectionController->openConnection(serverId);
        if (errorCode != ErrorCode::NoError) {
            logger.error() << "Failed to reopen connection, error code" << static_cast<int>(errorCode);
        }
    });
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
