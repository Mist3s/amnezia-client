#include "managedConfigUiController.h"

#include <QDateTime>
#include <QLocale>

#include "core/utils/managedConfig/managedConfigPolicy.h"

ManagedConfigUiController::ManagedConfigUiController(ManagedConfigController *managedConfigController, QObject *parent)
    : QObject(parent), m_managedConfigController(managedConfigController)
{
    connect(m_managedConfigController, &ManagedConfigController::refreshStarted, this, [this](const QString &serverId) {
        if (serverId == m_serverId) {
            emit statusChanged();
        }
    });

    connect(m_managedConfigController, &ManagedConfigController::refreshFinished, this,
            [this](const QString &serverId, ManagedConfigController::RefreshResult result) {
                if (serverId != m_serverId) {
                    return;
                }
                emit statusChanged();
                if (m_isManualRefresh) {
                    m_isManualRefresh = false;
                    emit manualRefreshFinished(resultText(result));
                }
            });

    connect(m_managedConfigController, &ManagedConfigController::setupFinished, this,
            [this](const QString &serverId, ManagedConfigController::RefreshResult result) {
                emit setupFinished(!serverId.isEmpty(), resultText(result));
            });
}

QString ManagedConfigUiController::backendUrls() const
{
    return m_managedConfigController->settings(m_serverId).backendUrls.join(", ");
}

QString ManagedConfigUiController::deviceKey() const
{
    return m_managedConfigController->settings(m_serverId).deviceKey;
}

QString ManagedConfigUiController::statusText() const
{
    const ManagedConfigSettings settings = m_managedConfigController->settings(m_serverId);
    if (!settings.isConfigured()) {
        return tr("Auto-update is off");
    }
    if (settings.lastCheckedAt == 0) {
        return tr("Not checked yet");
    }

    const QString checkedAt =
            QLocale().toString(QDateTime::fromSecsSinceEpoch(settings.lastCheckedAt), QLocale::ShortFormat);
    return tr("Last check: %1. %2")
            .arg(checkedAt, resultText(static_cast<ManagedConfigController::RefreshResult>(settings.lastResult)));
}

bool ManagedConfigUiController::isRefreshing() const
{
    return m_managedConfigController->isRefreshing(m_serverId);
}

bool ManagedConfigUiController::isSupported(const QString &serverId) const
{
    return m_managedConfigController->isSupported(serverId);
}

void ManagedConfigUiController::openServer(const QString &serverId)
{
    m_serverId = serverId;
    m_isManualRefresh = false;
    emit settingsChanged();
    emit statusChanged();
}

bool ManagedConfigUiController::save(const QString &backendUrls, const QString &deviceKey)
{
    const QStringList urls = amnezia::managedConfig::parseBackendUrls(backendUrls);
    const QString key = deviceKey.trimmed();

    const bool isTurnedOff = backendUrls.trimmed().isEmpty() && key.isEmpty();
    if (!isTurnedOff && (urls.isEmpty() || key.isEmpty())) {
        return false;
    }

    m_managedConfigController->configure(m_serverId, urls, key);
    emit settingsChanged();
    emit statusChanged();
    return true;
}

void ManagedConfigUiController::refresh()
{
    if (!m_managedConfigController->isManaged(m_serverId)) {
        return;
    }
    m_isManualRefresh = true;
    m_managedConfigController->refresh(m_serverId);
}

QString ManagedConfigUiController::resultText(ManagedConfigController::RefreshResult result) const
{
    switch (result) {
    case ManagedConfigController::RefreshResult::Updated: return tr("Config updated");
    case ManagedConfigController::RefreshResult::UpToDate: return tr("Config is up to date");
    case ManagedConfigController::RefreshResult::BackendUnreachable:
        return tr("Backend is unreachable, the last config stays in use");
    case ManagedConfigController::RefreshResult::KeyRejected: return tr("Backend rejected the device key");
    case ManagedConfigController::RefreshResult::InvalidConfig: return tr("Backend sent a config that cannot be used");
    }
    return QString();
}
