#include "backendUpdateController.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QtConcurrent>

#include "amneziaApplication.h"
#include "core/utils/managedConfig/managedConfigPolicy.h"
#include "logger.h"
#include "version.h"

#if defined(Q_OS_ANDROID)
    #include "platforms/android/android_controller.h"
#endif

namespace
{
    Logger logger("BackendUpdateController");

    constexpr int backendRequestTimeoutMs = 10 * 1000;
    constexpr int downloadStallTimeoutMs = 30 * 1000;
    constexpr qint64 backgroundCheckIntervalSecs = 6 * 60 * 60;
    constexpr int httpOk = 200;

#if defined(Q_OS_ANDROID)
    constexpr QLatin1String installerFileName("update.apk");
#elif defined(Q_OS_WINDOWS)
    constexpr QLatin1String installerFileName("update.exe");
#elif defined(Q_OS_LINUX)
    constexpr QLatin1String installerFileName("update.run");
#else
    constexpr QLatin1String installerFileName("");
#endif

    QString installerDirectory()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/updates";
    }

    QString installerFilePath()
    {
        return installerDirectory() + '/' + installerFileName;
    }

#if defined(Q_OS_WINDOWS) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    bool startInstallerAfterExit(const QString &installerPath)
    {
        const QString applicationPid = QString::number(QCoreApplication::applicationPid());
        QProcess process;
    #if defined(Q_OS_WINDOWS)
        process.setProgram("powershell.exe");
        process.setArguments(
                { "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command",
                  "Wait-Process -Id " + applicationPid
                          + " -ErrorAction SilentlyContinue; Start-Process -FilePath $env:HOMEVPN_INSTALLER" });
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOMEVPN_INSTALLER", installerPath);
        process.setProcessEnvironment(environment);
    #else
        QFile::setPermissions(installerPath, QFile::permissions(installerPath) | QFile::ExeUser);
        process.setProgram("/bin/sh");
        process.setArguments({ "-c", "while kill -0 \"$1\" 2>/dev/null; do sleep 0.5; done; exec \"$2\"", "sh",
                               applicationPid, installerPath });
    #endif
        return process.startDetached();
    }
#endif

    int httpStatusOf(QNetworkReply *reply)
    {
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }

    QNetworkRequest backendRequest(const QUrl &url, const QString &deviceKey, int transferTimeoutMs)
    {
        QNetworkRequest request(url);
        request.setTransferTimeout(transferTimeoutMs);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setRawHeader("Authorization", "Bearer " + deviceKey.toUtf8());
        return request;
    }
}

BackendUpdateController::BackendUpdateController(SecureAppSettingsRepository *appSettingsRepository,
                                                 ManagedConfigController *managedConfigController, QObject *parent)
    : UpdateController(appSettingsRepository, parent), m_managedConfigController(managedConfigController)
{
    QFile::remove(installerFilePath());

    const auto checkAfterBackendReply = [this](const QString &, ManagedConfigController::RefreshResult result) {
        checkAfterConfigRefresh(result);
    };
    connect(m_managedConfigController, &ManagedConfigController::refreshFinished, this, checkAfterBackendReply);
    connect(m_managedConfigController, &ManagedConfigController::setupFinished, this, checkAfterBackendReply);
}

bool BackendUpdateController::isBackendConfigured() const
{
    return !installerFileName.isEmpty() && m_managedConfigController->updateSource().isConfigured();
}

QNetworkAccessManager *BackendUpdateController::networkManager() const
{
    auto *application = qobject_cast<AmneziaApplication *>(QCoreApplication::instance());
    return application ? application->networkManager() : nullptr;
}

void BackendUpdateController::checkForUpdates()
{
    if (!isBackendConfigured()) {
        m_offer = { };
        UpdateController::checkForUpdates();
        return;
    }
    startCheck(false);
}

void BackendUpdateController::checkAfterConfigRefresh(ManagedConfigController::RefreshResult result)
{
    const bool isBackendReached = result == ManagedConfigController::RefreshResult::Updated
            || result == ManagedConfigController::RefreshResult::UpToDate;
    if (!isBackendReached || !isBackendConfigured() || !m_appSettingsRepository->isAutoUpdateCheckEnabled()) {
        return;
    }
    if (QDateTime::currentSecsSinceEpoch() - m_lastCheckedAt < backgroundCheckIntervalSecs) {
        return;
    }
    startCheck(true);
}

void BackendUpdateController::startCheck(bool isBackgroundCheck)
{
    if (isUpdateCheckRunning() || getUpdateState() == UpdateState::State::Downloading) {
        return;
    }

    const ManagedConfigSettings source = m_managedConfigController->updateSource();
    m_pendingBackendUrls = amnezia::managedConfig::orderedBackendUrls(source.backendUrls, source.lastWorkingUrl);
    m_isBackgroundCheck = isBackgroundCheck;
    m_lastCheckedAt = QDateTime::currentSecsSinceEpoch();

    setUpdateCheckRunning(true);
    requestNextBackend();
}

void BackendUpdateController::requestNextBackend()
{
    QNetworkAccessManager *manager = networkManager();
    const ManagedConfigSettings source = m_managedConfigController->updateSource();
    if (!manager || !source.isConfigured() || m_pendingBackendUrls.isEmpty()) {
        setUpdateCheckRunning(false);
        if (!m_isBackgroundCheck) {
            emit updateCheckFailed();
        }
        return;
    }

    const QString backendUrl = m_pendingBackendUrls.takeFirst();
    const QUrl endpoint = amnezia::managedUpdate::updateEndpoint(backendUrl, ManagedConfigController::thisDevice());

    QNetworkReply *reply = manager->get(backendRequest(endpoint, source.deviceKey, backendRequestTimeoutMs));
    connect(reply, &QNetworkReply::finished, this, [this, backendUrl, reply]() {
        reply->deleteLater();
        handleBackendReply(backendUrl, reply);
    });
}

void BackendUpdateController::handleBackendReply(const QString &backendUrl, QNetworkReply *reply)
{
    const amnezia::managedUpdate::ReplyDecision decision = amnezia::managedUpdate::decideOnReply(
            httpStatusOf(reply), reply->readAll(), backendUrl, QString(APP_VERSION));

    switch (decision.action) {
    case amnezia::managedUpdate::ReplyAction::TryNextBackend: {
        requestNextBackend();
        return;
    }
    case amnezia::managedUpdate::ReplyAction::RejectKey: {
        logger.warning() << "Backend rejected the device key";
        setUpdateCheckRunning(false);
        if (!m_isBackgroundCheck) {
            emit updateCheckFailed();
        }
        return;
    }
    case amnezia::managedUpdate::ReplyAction::NoUpdate: {
        setUpdateCheckRunning(false);
        if (!m_isBackgroundCheck) {
            emit updateNotFound();
        }
        return;
    }
    case amnezia::managedUpdate::ReplyAction::Offer: {
        setUpdateCheckRunning(false);
        announceOffer(decision.offer);
        return;
    }
    }
}

void BackendUpdateController::announceOffer(const amnezia::managedUpdate::UpdateOffer &offer)
{
    const bool isAlreadyAnnounced =
            m_offer.isValid() && m_offer.version == offer.version && m_offer.sha256 == offer.sha256;
    if (isAlreadyAnnounced && m_isBackgroundCheck) {
        return;
    }

    if (!isAlreadyAnnounced) {
        m_offer = offer;
        m_installerPath.clear();
        setUpdateState(UpdateState::State::Idle);
    }

    m_version = offer.version;
    m_releaseDate = offer.releaseDate;
    m_description.clear();
    m_tags.clear();
    m_newFeatures = offer.newFeatures;
    m_improvements = offer.improvements;
    m_bugFixes = offer.bugFixes;

    logger.info() << "Application update available:" << m_version;
    emit updateFound();
}

void BackendUpdateController::startUpdate()
{
    if (!m_offer.isValid()) {
        UpdateController::startUpdate();
        return;
    }
    if (getUpdateState() == UpdateState::State::Downloading) {
        return;
    }

    QNetworkAccessManager *manager = networkManager();
    const ManagedConfigSettings source = m_managedConfigController->updateSource();
    const QString filePath = installerFilePath();

    QDir().mkpath(installerDirectory());
    QFile::remove(filePath);
    m_installerPath.clear();

    auto *file = new QFile(filePath);
    if (!manager || !source.isConfigured() || !file->open(QIODevice::WriteOnly)) {
        delete file;
        logger.error() << "Update download cannot be started";
        setUpdateState(UpdateState::State::DownloadError);
        return;
    }

    setUpdateState(UpdateState::State::Downloading);

    QNetworkReply *reply = manager->get(backendRequest(m_offer.fileUrl, source.deviceKey, downloadStallTimeoutMs));
    file->setParent(reply);

    connect(reply, &QNetworkReply::readyRead, file, [reply, file]() { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, filePath]() {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();

        if (reply->error() != QNetworkReply::NoError || httpStatusOf(reply) != httpOk) {
            logger.error() << "Update download failed, network error:" << static_cast<int>(reply->error())
                           << "HTTP status:" << httpStatusOf(reply);
            failDownload(filePath);
            return;
        }
        verifyDownload(filePath);
    });
}

void BackendUpdateController::failDownload(const QString &filePath)
{
    QFile::remove(filePath);
    setUpdateState(UpdateState::State::DownloadError);
}

void BackendUpdateController::verifyDownload(const QString &filePath)
{
    const QString expectedSha256 = m_offer.sha256;

    QtConcurrent::run(amnezia::managedUpdate::fileMatchesSha256, filePath, expectedSha256)
            .then(this, [this, filePath, expectedSha256](bool isMatching) {
                if (!isMatching || expectedSha256 != m_offer.sha256) {
                    logger.error() << "Downloaded update does not match sha256 from the backend manifest";
                    failDownload(filePath);
                    return;
                }

                m_installerPath = filePath;
                setUpdateState(UpdateState::State::ReadyToInstall);
#if defined(Q_OS_ANDROID)
                installUpdate();
#endif
            });
}

void BackendUpdateController::installUpdate()
{
    if (!m_offer.isValid()) {
        UpdateController::installUpdate();
        return;
    }
    if (getUpdateState() != UpdateState::State::ReadyToInstall || m_installerPath.isEmpty()) {
        return;
    }

#if defined(Q_OS_ANDROID)
    AndroidController::instance()->installApk(m_installerPath);
#elif defined(Q_OS_WINDOWS) || defined(Q_OS_LINUX)
    if (!startInstallerAfterExit(m_installerPath)) {
        logger.error() << "Failed to start the installer";
        return;
    }
    logger.info() << "Quitting so that the installer can replace the application";
    QCoreApplication::quit();
#endif
}
