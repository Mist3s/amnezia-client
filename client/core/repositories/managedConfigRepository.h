#ifndef MANAGEDCONFIGREPOSITORY_H
#define MANAGEDCONFIGREPOSITORY_H

#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include "secureQSettings.h"

struct ManagedConfigSettings
{
    QStringList backendUrls;
    QString deviceKey;
    QString revision;
    QString listRevision;
    QString mainId;
    QString remoteId;
    QString remoteName;
    QString lastWorkingUrl;
    int pollIntervalHours = 0;
    qint64 lastCheckedAt = 0;
    int lastResult = -1;

    bool isConfigured() const;
};

class ManagedConfigRepository : public QObject
{
    Q_OBJECT

public:
    explicit ManagedConfigRepository(SecureQSettings *settings, QObject *parent = nullptr);

    ManagedConfigSettings settings(const QString &serverId) const;
    void setSettings(const QString &serverId, const ManagedConfigSettings &settings);
    void remove(const QString &serverId);

    QStringList managedServerIds() const;

private:
    QVariantMap storedSettings() const;

    SecureQSettings *m_settings;
};

#endif // MANAGEDCONFIGREPOSITORY_H
