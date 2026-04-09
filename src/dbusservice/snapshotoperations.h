#ifndef SNAPSHOTOPERATIONS_H
#define SNAPSHOTOPERATIONS_H

#include <memory>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QDBusContext>
#include <QTimer>

namespace snapper {
    class Snapper;
}

class SnapshotOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

private:
    static constexpr int IdleTimeoutMs = 5 * 60 * 1000; // アイドルタイム: 5分
    std::unique_ptr<snapper::Snapper> m_snapper;        // Snapperインスタンス
    QString m_currentConfig;                            // 現在の設定名
    QTimer m_idleTimer;                                 // アイドルタイムアウト用タイマー
    bool m_authenticated;                               // 事前認証済みフラグ

    void resetIdleTimer();

public:
    explicit SnapshotOperations(QObject *parent = nullptr);
    ~SnapshotOperations();

public slots:
    bool Authenticate(const QString &actionId);
    QStringList ListConfigs();
    QString ListSnapshots(const QString &configName);
    QString CreateSnapshot(const QString &configName, const QString &type, const QString &description,
                          int preNumber, const QString &cleanup,
                          const QMap<QString, QString> &userdata, bool important);
    bool ModifySnapshot(const QString &configName, int number, const QString &description,
                        const QString &cleanup, const QMap<QString, QString> &userdata);
    bool DeleteSnapshot(const QString &configName, int number);
    bool RollbackSnapshot(const QString &configName, int number);
    QString GetFileChanges(const QString &configName, int snapshotNumber);
    QString GetFileChangesBetween(const QString &configName, int number1, int number2);
    QString GetFileDiffAndDetails(const QString &configName, int snapshotNumber, const QString &filePath);
    QString GetFileDiffBetween(const QString &configName, int number1, int number2, const QString &filePath);
    bool RestoreFiles(const QString &configName, int snapshotNumber, const QStringList &filePaths, const QStringList &changeTypes);
    bool RestoreFilesDirect(const QString &configName, int snapshotNumber, const QStringList &filePaths, const QStringList &changeTypes);
    void Quit();

signals:
    void restoreProgress(int current, int total, const QString &filePath);

private:
    bool checkAuthorization(const QString &actionId);
    snapper::Snapper* getSnapper(const QString &configName = "root");
    QString formatSnapshotToCSV(const snapper::Snapper *snapper);
    QString snapshotTypeToString(int type);
    int stringToSnapshotType(const QString &typeStr);
};

#endif // SNAPSHOTOPERATIONS_H
