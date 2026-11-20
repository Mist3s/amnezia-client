#ifndef BACKENDUPDATECONTROLLER_H
#define BACKENDUPDATECONTROLLER_H

#include <QStringList>

#include "core/controllers/managedConfigController.h"
#include "core/controllers/updateController.h"
#include "core/utils/managedConfig/managedUpdatePolicy.h"

class QNetworkAccessManager;
class QNetworkReply;

class BackendUpdateController : public UpdateController
{
    Q_OBJECT

public:
    explicit BackendUpdateController(SecureAppSettingsRepository *appSettingsRepository,
                                     ManagedConfigController *managedConfigController, QObject *parent = nullptr);

public slots:
    void checkForUpdates() override;
    void startUpdate() override;
    void installUpdate() override;

private:
    bool isBackendConfigured() const;
    QNetworkAccessManager *networkManager() const;

    void checkAfterConfigRefresh(ManagedConfigController::RefreshResult result);
    void startCheck(bool isBackgroundCheck);
    void requestNextBackend();
    void handleBackendReply(const QString &backendUrl, QNetworkReply *reply);
    void announceOffer(const amnezia::managedUpdate::UpdateOffer &offer);

    void failDownload(const QString &filePath);
    void verifyDownload(const QString &filePath);

    ManagedConfigController *m_managedConfigController;

    QStringList m_pendingBackendUrls;
    bool m_isBackgroundCheck = false;
    qint64 m_lastCheckedAt = 0;

    amnezia::managedUpdate::UpdateOffer m_offer;
    QString m_installerPath;
};

#endif // BACKENDUPDATECONTROLLER_H
