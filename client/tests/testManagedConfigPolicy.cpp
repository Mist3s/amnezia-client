#include <QJsonArray>
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

    static QJsonObject listEntry(const QString &id, const QString &revision, const QString &name = QString())
    {
        return QJsonObject { { "id", id }, { "name", name }, { "revision", revision }, { "config", "[Interface]\n" } };
    }

    static QByteArray listBody(const QString &revision, const QString &mainId, const QJsonArray &servers)
    {
        return QJsonDocument(QJsonObject { { "revision", revision },
                                           { "main", mainId },
                                           { "servers", servers },
                                           { "poll_interval_hours", 6 } })
                .toJson();
    }

    static ReplyDecision listDecision(const QString &mainId, const QJsonArray &servers)
    {
        return decideOnReply(200, listBody("list2", mainId, servers), "list1");
    }

    static QStringList remoteIds(const QList<RemoteServer> &servers)
    {
        QStringList ids;
        for (const RemoteServer &server : servers) {
            ids.append(server.id);
        }
        return ids;
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
        QCOMPARE(configEndpoint("https://example.org/base", {}).toString(),
                 QString("https://example.org/base/v1/config?servers=1"));
    }

    void configEndpointTellsWhatTheDeviceRuns()
    {
        QCOMPARE(configEndpoint("https://example.org/base", { "android", "5.1.1.1110", "arm64-v8a", 34 }).toString(),
                 QString("https://example.org/base/v1/"
                         "config?servers=1&platform=android&version=5.1.1.1110&abi=arm64-v8a&sdk=34"));
        QCOMPARE(configEndpoint("https://example.org/base", { "linux", "5.1.1.1110", "x86_64", 0 }).toString(),
                 QString("https://example.org/base/v1/config?servers=1&platform=linux&version=5.1.1.1110&abi=x86_64"));
    }

    void androidAbiIsNamedAsInApk()
    {
        QCOMPARE(androidAbi("arm64"), QString("arm64-v8a"));
        QCOMPARE(androidAbi("arm"), QString("armeabi-v7a"));
        QCOMPARE(androidAbi("mips"), QString());
    }

    void newRevisionIsApplied()
    {
        const ReplyDecision decision = decideOnReply(200, replyBody("rev2", "[Interface]\n", 3), "rev1");
        QCOMPARE(decision.action, ReplyAction::Apply);
        QCOMPARE(decision.revision, QString("rev2"));
        QCOMPARE(decision.servers.size(), 1);
        QCOMPARE(decision.servers.first().config, QString("[Interface]\n"));
        QCOMPARE(decision.servers.first().revision, QString("rev2"));
        QCOMPARE(decision.pollIntervalHours, 3);
        QVERIFY(!decision.isServerList);
    }

    void serverListIsReadWithItsMainServer()
    {
        const ReplyDecision decision =
                listDecision("fi", { listEntry("fi", "a", " Finland  home "), listEntry("nl", "b") });
        QCOMPARE(decision.action, ReplyAction::Apply);
        QVERIFY(decision.isServerList);
        QCOMPARE(decision.mainId, QString("fi"));
        QCOMPARE(remoteIds(decision.servers), QStringList({ "fi", "nl" }));
        QCOMPARE(decision.servers.first().name, QString("Finland home"));
        QCOMPARE(decideOnReply(200, listBody("list1", "fi", { listEntry("fi", "a") }), "list1").action,
                 ReplyAction::KeepCurrent);
    }

    void brokenServerListMovesToNextBackend_data()
    {
        QTest::addColumn<QString>("mainId");
        QTest::addColumn<QJsonArray>("servers");

        QTest::newRow("empty list") << "fi" << QJsonArray();
        QTest::newRow("main is not in the list") << "se" << QJsonArray { listEntry("fi", "a") };
        QTest::newRow("no main") << "" << QJsonArray { listEntry("fi", "a") };
        QTest::newRow("entry without id") << "fi" << QJsonArray { listEntry("fi", "a"), listEntry("", "b") };
        QTest::newRow("entry without revision") << "fi" << QJsonArray { listEntry("fi", "a"), listEntry("nl", "") };
        QTest::newRow("repeated id") << "fi" << QJsonArray { listEntry("fi", "a"), listEntry("fi", "b") };
    }

    void brokenServerListMovesToNextBackend()
    {
        QFETCH(QString, mainId);
        QFETCH(QJsonArray, servers);

        QCOMPARE(listDecision(mainId, servers).action, ReplyAction::TryNextBackend);
    }

    void firstListAddsEveryServer()
    {
        const ServerListChanges changes =
                planServerListChanges({}, QString(), listDecision("fi", { listEntry("fi", "a"), listEntry("nl", "b") }));
        QCOMPARE(remoteIds(changes.added), QStringList({ "fi", "nl" }));
        QVERIFY(changes.kept.isEmpty() && changes.removedServerIds.isEmpty() && !changes.isMainChanged);
        QCOMPARE(changes.mainId, QString("fi"));
    }

    void serverFromBeforeTheListBecomesItsMainServer()
    {
        const ServerListChanges changes =
                planServerListChanges({ { "local1", QString(), "a" } }, QString(),
                                      listDecision("fi", { listEntry("fi", "a"), listEntry("nl", "b") }));
        QCOMPARE(changes.kept.size(), 1);
        QCOMPARE(changes.kept.first().serverId, QString("local1"));
        QCOMPARE(changes.kept.first().remote.id, QString("fi"));
        QVERIFY(!changes.kept.first().isConfigChanged);
        QCOMPARE(remoteIds(changes.added), QStringList({ "nl" }));
        QVERIFY(changes.removedServerIds.isEmpty() && !changes.isMainChanged);
    }

    void listChangesAreSplitIntoKeptAddedAndRemoved()
    {
        const QList<LocalServer> local = { { "local1", "fi", "a" }, { "local2", "nl", "b" }, { "local3", "se", "c" } };
        const ServerListChanges changes = planServerListChanges(
                local, "fi", listDecision("fi", { listEntry("fi", "a"), listEntry("nl", "b2"), listEntry("de", "d") }));
        QCOMPARE(changes.kept.size(), 2);
        QVERIFY(!changes.kept.at(0).isConfigChanged);
        QCOMPARE(changes.kept.at(1).serverId, QString("local2"));
        QVERIFY(changes.kept.at(1).isConfigChanged);
        QCOMPARE(remoteIds(changes.added), QStringList({ "de" }));
        QCOMPARE(changes.removedServerIds, QStringList({ "local3" }));
        QVERIFY(!changes.isMainChanged);
    }

    void newMainServerIsNoticed()
    {
        const QList<LocalServer> local = { { "local1", "fi", "a" }, { "local2", "nl", "b" } };
        const ServerListChanges changes =
                planServerListChanges(local, "fi", listDecision("nl", { listEntry("nl", "b"), listEntry("fi", "a") }));
        QVERIFY(changes.isMainChanged);
        QCOMPARE(changes.mainId, QString("nl"));
        QVERIFY(changes.added.isEmpty() && changes.removedServerIds.isEmpty());
    }

    void serverBoundByHandIsNeverRemoved()
    {
        const QList<LocalServer> local = { { "local1", "fi", "a" }, { "byHand", QString(), "x" } };
        const ServerListChanges changes =
                planServerListChanges(local, "fi", listDecision("fi", { listEntry("fi", "a") }));
        QCOMPARE(changes.kept.size(), 1);
        QVERIFY(changes.removedServerIds.isEmpty());
    }

    void singleConfigReplyUpdatesTheMainServerOnly()
    {
        const QList<LocalServer> local = { { "local1", "fi", "a" }, { "local2", "nl", "b" } };
        const ServerListChanges changes =
                planServerListChanges(local, "fi", decideOnReply(200, replyBody("a2", "[Interface]\n"), "list1"));
        QCOMPARE(changes.kept.size(), 1);
        QCOMPARE(changes.kept.first().serverId, QString("local1"));
        QVERIFY(changes.kept.first().isConfigChanged);
        QVERIFY(changes.added.isEmpty() && changes.removedServerIds.isEmpty() && !changes.isMainChanged);

        const ServerListChanges legacy = planServerListChanges({ { "old", QString(), "a" } }, QString(),
                                                               decideOnReply(200, replyBody("a", "x"), "b"));
        QCOMPARE(legacy.kept.size(), 1);
        QVERIFY(!legacy.kept.first().isConfigChanged && legacy.added.isEmpty());
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
