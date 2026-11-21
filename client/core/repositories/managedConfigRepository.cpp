#include "managedConfigRepository.h"

namespace
{
    constexpr QLatin1String storageKey("Conf/managedConfigs");

    constexpr QLatin1String backendUrlsKey("backendUrls");
    constexpr QLatin1String deviceKeyKey("deviceKey");
    constexpr QLatin1String revisionKey("revision");
    constexpr QLatin1String listRevisionKey("listRevision");
    constexpr QLatin1String mainIdKey("mainId");
    constexpr QLatin1String remoteIdKey("remoteId");
    constexpr QLatin1String remoteNameKey("remoteName");
    constexpr QLatin1String lastWorkingUrlKey("lastWorkingUrl");
    constexpr QLatin1String pollIntervalHoursKey("pollIntervalHours");
    constexpr QLatin1String lastCheckedAtKey("lastCheckedAt");
    constexpr QLatin1String lastResultKey("lastResult");
}

bool ManagedConfigSettings::isConfigured() const
{
    return !backendUrls.isEmpty() && !deviceKey.isEmpty();
}

ManagedConfigRepository::ManagedConfigRepository(SecureQSettings *settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
}

ManagedConfigSettings ManagedConfigRepository::settings(const QString &serverId) const
{
    const QVariantMap stored = storedSettings().value(serverId).toMap();

    ManagedConfigSettings settings;
    settings.backendUrls = stored.value(backendUrlsKey).toStringList();
    settings.deviceKey = stored.value(deviceKeyKey).toString();
    settings.revision = stored.value(revisionKey).toString();
    settings.listRevision = stored.value(listRevisionKey).toString();
    settings.mainId = stored.value(mainIdKey).toString();
    settings.remoteId = stored.value(remoteIdKey).toString();
    settings.remoteName = stored.value(remoteNameKey).toString();
    settings.lastWorkingUrl = stored.value(lastWorkingUrlKey).toString();
    settings.pollIntervalHours = stored.value(pollIntervalHoursKey).toInt();
    settings.lastCheckedAt = stored.value(lastCheckedAtKey).toLongLong();
    settings.lastResult = stored.value(lastResultKey, -1).toInt();
    return settings;
}

void ManagedConfigRepository::setSettings(const QString &serverId, const ManagedConfigSettings &settings)
{
    if (serverId.isEmpty()) {
        return;
    }

    QVariantMap stored;
    stored.insert(backendUrlsKey, settings.backendUrls);
    stored.insert(deviceKeyKey, settings.deviceKey);
    stored.insert(revisionKey, settings.revision);
    stored.insert(listRevisionKey, settings.listRevision);
    stored.insert(mainIdKey, settings.mainId);
    stored.insert(remoteIdKey, settings.remoteId);
    stored.insert(remoteNameKey, settings.remoteName);
    stored.insert(lastWorkingUrlKey, settings.lastWorkingUrl);
    stored.insert(pollIntervalHoursKey, settings.pollIntervalHours);
    stored.insert(lastCheckedAtKey, settings.lastCheckedAt);
    stored.insert(lastResultKey, settings.lastResult);

    QVariantMap all = storedSettings();
    all.insert(serverId, stored);
    m_settings->setValue(storageKey, all);
}

void ManagedConfigRepository::remove(const QString &serverId)
{
    QVariantMap all = storedSettings();
    if (all.remove(serverId) > 0) {
        m_settings->setValue(storageKey, all);
    }
}

QStringList ManagedConfigRepository::managedServerIds() const
{
    QStringList serverIds;
    const QVariantMap all = storedSettings();
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        if (settings(it.key()).isConfigured()) {
            serverIds.append(it.key());
        }
    }
    return serverIds;
}

QVariantMap ManagedConfigRepository::storedSettings() const
{
    return m_settings->value(storageKey).toMap();
}
