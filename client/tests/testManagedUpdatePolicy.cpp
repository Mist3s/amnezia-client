#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "core/utils/managedConfig/managedUpdatePolicy.h"

using namespace amnezia::managedUpdate;

namespace
{
    const QString backendUrl = "https://backend.example.org/base";
    const QString sha256OfAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

class TestManagedUpdatePolicy : public QObject
{
    Q_OBJECT

private:
    static QByteArray offerBody(const QString &version, const QString &url, const QString &sha256)
    {
        return QJsonDocument(QJsonObject { { "update_available", true },
                                           { "version", version },
                                           { "url", url },
                                           { "sha256", sha256 } })
                .toJson();
    }

    QString writeFile(const QByteArray &content)
    {
        const QString path = m_directory.filePath("update.bin");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
            return QString();
        }
        return path;
    }

    QTemporaryDir m_directory;

private slots:
    void fileWithExpectedSha256IsAccepted()
    {
        QVERIFY(fileMatchesSha256(writeFile("abc"), sha256OfAbc));
    }

    void changedFileIsRejected()
    {
        QVERIFY(!fileMatchesSha256(writeFile("abd"), sha256OfAbc));
        QVERIFY(!fileMatchesSha256(writeFile("abc "), sha256OfAbc));
        QVERIFY(!fileMatchesSha256(writeFile(QByteArray()), sha256OfAbc));
    }

    void missingFileIsRejected()
    {
        QVERIFY(!fileMatchesSha256(m_directory.filePath("absent.bin"), sha256OfAbc));
    }

    void malformedSha256IsRejected()
    {
        const QString path = writeFile("abc");
        QVERIFY(!fileMatchesSha256(path, QString()));
        QVERIFY(!fileMatchesSha256(path, sha256OfAbc.left(63)));
        QVERIFY(!fileMatchesSha256(path, sha256OfAbc.toUpper()));
        QVERIFY(!fileMatchesSha256(path, QString(64, 'z')));
    }

    void newerVersionIsOffered()
    {
        const ReplyDecision decision = decideOnReply(
                200, offerBody("5.2.0.1", "v1/files/app.apk", sha256OfAbc.toUpper()), backendUrl, "5.1.1.11");
        QCOMPARE(decision.action, ReplyAction::Offer);
        QCOMPARE(decision.offer.version, QString("5.2.0.1"));
        QCOMPARE(decision.offer.fileUrl.toString(), QString("https://backend.example.org/base/v1/files/app.apk"));
        QCOMPARE(decision.offer.sha256, sha256OfAbc);
    }

    void sameOrOlderVersionIsNotOffered()
    {
        QCOMPARE(decideOnReply(200, offerBody("5.1.1.11", "v1/files/app.apk", sha256OfAbc), backendUrl, "5.1.1.11").action,
                 ReplyAction::NoUpdate);
        QCOMPARE(decideOnReply(200, offerBody("5.1.1.9", "v1/files/app.apk", sha256OfAbc), backendUrl, "5.1.1.11").action,
                 ReplyAction::NoUpdate);
        QCOMPARE(decideOnReply(200, QByteArray("{\"update_available\": false}"), backendUrl, "5.1.1.11").action,
                 ReplyAction::NoUpdate);
    }

    void rejectedKeyStopsTheSearch()
    {
        QCOMPARE(decideOnReply(401, QByteArray(), backendUrl, "5.1.1.11").action, ReplyAction::RejectKey);
        QCOMPARE(decideOnReply(403, QByteArray(), backendUrl, "5.1.1.11").action, ReplyAction::RejectKey);
    }

    void unusableReplyMovesToNextBackend_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("body");

        QTest::newRow("no response") << 0 << QByteArray();
        QTest::newRow("server error") << 500 << QByteArray();
        QTest::newRow("redirect") << 302 << QByteArray();
        QTest::newRow("not json") << 200 << QByteArray("<html></html>");
        QTest::newRow("no flag") << 200 << QByteArray("{}");
        QTest::newRow("no version") << 200 << offerBody(QString(), "v1/files/app.apk", sha256OfAbc);
        QTest::newRow("no sha256") << 200 << offerBody("5.2.0", "v1/files/app.apk", QString());
        QTest::newRow("short sha256") << 200 << offerBody("5.2.0", "v1/files/app.apk", sha256OfAbc.left(40));
        QTest::newRow("no file") << 200 << offerBody("5.2.0", QString(), sha256OfAbc);
        QTest::newRow("file on another host")
                << 200 << offerBody("5.2.0", "https://evil.example.org/app.apk", sha256OfAbc);
        QTest::newRow("file without tls") << 200 << offerBody("5.2.0", "http://backend.example.org/app.apk", sha256OfAbc);
    }

    void unusableReplyMovesToNextBackend()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, body);

        QCOMPARE(decideOnReply(httpStatus, body, backendUrl, "5.1.1.11").action, ReplyAction::TryNextBackend);
    }

    void fileUrlStaysOnBackend()
    {
        QCOMPARE(fileUrl(backendUrl, "v1/files/app.apk").toString(),
                 QString("https://backend.example.org/base/v1/files/app.apk"));
        QCOMPARE(fileUrl(backendUrl, "https://backend.example.org/other/app.apk").toString(),
                 QString("https://backend.example.org/other/app.apk"));
        QVERIFY(fileUrl(backendUrl, "//evil.example.org/app.apk").isEmpty());
        QVERIFY(fileUrl(backendUrl, "https://backend.example.org:8443/app.apk").isEmpty());
        QVERIFY(fileUrl(backendUrl, "https://user:secret@backend.example.org/app.apk").isEmpty());
    }

    void versionsAreComparedByNumbers()
    {
        QVERIFY(isNewerVersion("5.1.1.12", "5.1.1.11"));
        QVERIFY(isNewerVersion("5.1.10", "5.1.9.99"));
        QVERIFY(isNewerVersion("5.1.1.11.1", "5.1.1.11"));
        QVERIFY(!isNewerVersion("5.1.1.11", "5.1.1.11"));
        QVERIFY(!isNewerVersion("5.1.1", "5.1.1.11"));
        QVERIFY(!isNewerVersion("latest", "5.1.1.11"));
        QVERIFY(!isNewerVersion("5.2.0-beta", "5.1.1.11"));
    }

    void updateEndpointCarriesDeviceDescription()
    {
        QCOMPARE(updateEndpoint(backendUrl, "android", "5.1.1.11", "arm64-v8a", 34).toString(),
                 QString("https://backend.example.org/base/v1/"
                         "app_update?platform=android&version=5.1.1.11&abi=arm64-v8a&sdk=34"));
        QCOMPARE(updateEndpoint(backendUrl, "linux", "5.1.1.11", "x86_64", 0).toString(),
                 QString("https://backend.example.org/base/v1/app_update?platform=linux&version=5.1.1.11&abi=x86_64"));
    }

    void androidAbiIsNamedAsInApk()
    {
        QCOMPARE(androidAbi("arm64"), QString("arm64-v8a"));
        QCOMPARE(androidAbi("arm"), QString("armeabi-v7a"));
        QCOMPARE(androidAbi("mips"), QString());
    }
};

QTEST_MAIN(TestManagedUpdatePolicy)
#include "testManagedUpdatePolicy.moc"
