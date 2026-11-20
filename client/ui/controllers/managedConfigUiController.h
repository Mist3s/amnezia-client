#ifndef MANAGEDCONFIGUICONTROLLER_H
#define MANAGEDCONFIGUICONTROLLER_H

#include <QObject>

#include "core/controllers/managedConfigController.h"

class ManagedConfigUiController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString backendUrls READ backendUrls NOTIFY settingsChanged)
    Q_PROPERTY(QString deviceKey READ deviceKey NOTIFY settingsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool isRefreshing READ isRefreshing NOTIFY statusChanged)

public:
    explicit ManagedConfigUiController(ManagedConfigController *managedConfigController, QObject *parent = nullptr);

    QString backendUrls() const;
    QString deviceKey() const;
    QString statusText() const;
    bool isRefreshing() const;

public slots:
    bool isSupported(const QString &serverId) const;
    void openServer(const QString &serverId);
    bool save(const QString &backendUrls, const QString &deviceKey);
    void refresh();

signals:
    void settingsChanged();
    void statusChanged();
    void manualRefreshFinished(const QString &message);
    void setupStarted();
    void setupFinished(bool isSetUp, const QString &message);

private:
    QString resultText(ManagedConfigController::RefreshResult result) const;

    ManagedConfigController *m_managedConfigController;
    QString m_serverId;
    bool m_isManualRefresh = false;
};

#endif // MANAGEDCONFIGUICONTROLLER_H
