#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "core/utils/managedConfig/managedConfigPolicy.h"

using namespace amnezia::managedConfig;

class TestManagedConfigPolicy : public QObject
{
    Q_OBJECT

private:
    static QByteArray replyBody(const QString &revision, const QString &config, int pollIntervalHours = 6)
    {
        return QJsonDocument(QJsonObject { { "revision", revision },
                                           { "config", config },
                                           { "poll_interval_hours", pollIntervalHours } })
                .toJson();
    }

    static QByteArray setupFile(const QJsonObject &content)
    {
        return QJsonDocument(content).toJson();
    }

private slots:
    void backendUrlsAreSplitAndNormalized()
    {
        const QStringList urls =
                parseBackendUrls(" https://One.example.org/ ,two.example.org:8443;\nhttps://three.example.org/base/ ");
        QCOMPARE(urls,
                 QStringList({ "https://one.example.org", "https://two.example.org:8443",
                               "https://three.example.org/base" }));
    }

    void backendUrlsWithoutTlsAreDropped()
    {
        QCOMPARE(parseBackendUrls("http://plain.example.org, ftp://files.example.org"), QStringList());
        QCOMPARE(parseBackendUrls("http://plain.example.org https://safe.example.org"),
                 QStringList({ "https://safe.example.org" }));
    }

    void backendUrlsLoseQueryAndCredentials()
    {
        QCOMPARE(parseBackendUrls("https://user:secret@example.org/api?token=1#part"),
                 QStringList({ "https://example.org/api" }));
    }

    void backendUrlsAreNotRepeated()
    {
        QCOMPARE(parseBackendUrls("https://example.org, example.org, https://example.org/"),
                 QStringList({ "https://example.org" }));
    }

    void setupFileGivesBackendAndKey()
    {
        const SetupRequest request = parseSetupRequest(setupFile({ { "backend", "https://One.example.org/, two.example.org" },
                                                                   { "key", " device-key " },
                                                                   { "connect", true },
                                                                   { "autostart", true } }));
        QVERIFY(request.isValid());
        QCOMPARE(request.backendUrls, QStringList({ "https://one.example.org", "https://two.example.org" }));
        QCOMPARE(request.deviceKey, QString("device-key"));
        QVERIFY(request.connect);
        QVERIFY(request.autoStart);
    }

    void setupFileWithByteOrderMarkIsRead()
    {
        const SetupRequest request = parseSetupRequest(
                QByteArray("\xEF\xBB\xBF") + setupFile({ { "backend", "https://example.org" }, { "key", "device-key" } }));
        QVERIFY(request.isValid());
        QVERIFY(!request.connect);
        QVERIFY(!request.autoStart);
    }

    void setupFileWithoutBackendOrKeyIsInvalid_data()
    {
        QTest::addColumn<QByteArray>("content");

        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not json") << QByteArray("backend=https://example.org");
        QTest::newRow("no key") << setupFile({ { "backend", "https://example.org" } });
        QTest::newRow("blank key") << setupFile({ { "backend", "https://example.org" }, { "key", "  " } });
        QTest::newRow("no backend") << setupFile({ { "key", "device-key" } });
        QTest::newRow("backend without tls") << setupFile({ { "backend", "http://example.org" }, { "key", "device-key" } });
    }

    void setupFileWithoutBackendOrKeyIsInvalid()
    {
        QFETCH(QByteArray, content);
        QVERIFY(!parseSetupRequest(content).isValid());
    }

    void backendsInCommonAreFound()
    {
        QVERIFY(haveCommonBackend({ "https://one.example.org", "https://two.example.org" }, { "https://two.example.org" }));
        QVERIFY(!haveCommonBackend({ "https://one.example.org" }, { "https://two.example.org" }));
        QVERIFY(!haveCommonBackend({}, { "https://two.example.org" }));
    }

    void emptyInputGivesNoBackends()
    {
        QCOMPARE(parseBackendUrls(" , ;\n"), QStringList());
    }

    void lastWorkingBackendGoesFirst()
    {
        const QStringList urls = { "https://a.example.org", "https://b.example.org", "https://c.example.org" };
        QCOMPARE(orderedBackendUrls(urls, "https://c.example.org"),
                 QStringList({ "https://c.example.org", "https://a.example.org", "https://b.example.org" }));
    }

    void unknownLastWorkingBackendKeepsOrder()
    {
        const QStringList urls = { "https://a.example.org", "https://b.example.org" };
        QCOMPARE(orderedBackendUrls(urls, QString()), urls);
        QCOMPARE(orderedBackendUrls(urls, "https://gone.example.org"), urls);
    }

    void configEndpointKeepsBackendPath()
    {
        QCOMPARE(configEndpoint("https://example.org/base").toString(), QString("https://example.org/base/v1/config"));
    }

    void newRevisionIsApplied()
    {
        const ReplyDecision decision = decideOnReply(200, replyBody("rev2", "[Interface]\n", 3), "rev1");
        QCOMPARE(decision.action, ReplyAction::Apply);
        QCOMPARE(decision.revision, QString("rev2"));
        QCOMPARE(decision.config, QString("[Interface]\n"));
        QCOMPARE(decision.pollIntervalHours, 3);
    }

    void firstConfigIsApplied()
    {
        QCOMPARE(decideOnReply(200, replyBody("rev1", "[Interface]\n"), QString()).action, ReplyAction::Apply);
    }

    void sameRevisionKeepsCurrentConfig()
    {
        QCOMPARE(decideOnReply(200, replyBody("rev1", "[Interface]\n"), "rev1").action, ReplyAction::KeepCurrent);
        QCOMPARE(decideOnReply(304, QByteArray(), "rev1").action, ReplyAction::KeepCurrent);
    }

    void rejectedKeyStopsTheSearch()
    {
        QCOMPARE(decideOnReply(401, QByteArray(), "rev1").action, ReplyAction::RejectKey);
        QCOMPARE(decideOnReply(403, QByteArray(), "rev1").action, ReplyAction::RejectKey);
    }

    void unusableReplyMovesToNextBackend_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("body");

        QTest::newRow("no response") << 0 << QByteArray();
        QTest::newRow("server error") << 500 << QByteArray();
        QTest::newRow("no active server") << 503 << QByteArray();
        QTest::newRow("redirect") << 302 << QByteArray();
        QTest::newRow("not found") << 404 << QByteArray();
        QTest::newRow("not json") << 200 << QByteArray("<html></html>");
        QTest::newRow("no revision") << 200 << replyBody(QString(), "[Interface]\n");
        QTest::newRow("empty config") << 200 << replyBody("rev2", " \n");
    }

    void unusableReplyMovesToNextBackend()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, body);

        QCOMPARE(decideOnReply(httpStatus, body, "rev1").action, ReplyAction::TryNextBackend);
    }

    void pollIntervalIsBounded()
    {
        QCOMPARE(normalizedPollIntervalHours(0), defaultPollIntervalHours);
        QCOMPARE(normalizedPollIntervalHours(-5), defaultPollIntervalHours);
        QCOMPARE(normalizedPollIntervalHours(12), 12);
        QCOMPARE(normalizedPollIntervalHours(100000), maxPollIntervalHours);
    }

    void pollDelayIsSpreadAroundInterval()
    {
        const qint64 hourMs = 60 * 60 * 1000;
        QCOMPARE(nextPollDelayMs(10, 0.5), 10 * hourMs);
        QCOMPARE(nextPollDelayMs(10, 0.0), 8 * hourMs);
        QCOMPARE(nextPollDelayMs(10, 1.0), 12 * hourMs);
        QCOMPARE(nextPollDelayMs(10, 7.0), 12 * hourMs);
    }
};

QTEST_MAIN(TestManagedConfigPolicy)
#include "testManagedConfigPolicy.moc"
