#ifndef MANAGEDUPDATEPOLICY_H
#define MANAGEDUPDATEPOLICY_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace amnezia::managedUpdate
{

    enum class ReplyAction {
        Offer,
        NoUpdate,
        RejectKey,
        TryNextBackend
    };

    struct UpdateOffer
    {
        QString version;
        QUrl fileUrl;
        QString sha256;
        QString releaseDate;
        QStringList newFeatures;
        QStringList improvements;
        QStringList bugFixes;

        bool isValid() const;
    };

    struct ReplyDecision
    {
        ReplyAction action = ReplyAction::TryNextBackend;
        UpdateOffer offer;
    };

    QString androidAbi(const QString &cpuArchitecture);

    QUrl updateEndpoint(const QString &backendUrl, const QString &platform, const QString &version, const QString &abi,
                        int sdk);
    QUrl fileUrl(const QString &backendUrl, const QString &filePath);

    bool isNewerVersion(const QString &candidate, const QString &current);

    ReplyDecision decideOnReply(int httpStatus, const QByteArray &body, const QString &backendUrl,
                                const QString &currentVersion);

    bool fileMatchesSha256(const QString &filePath, const QString &expectedSha256);

} // namespace amnezia::managedUpdate

#endif // MANAGEDUPDATEPOLICY_H
