#include "snapshotoperations.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDateTime>
#include <QProcess>
#include <QFileInfo>
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <snapper/Version.h>

// 古いlibsnapper (7.x未満) には LIBSNAPPER_VERSION_AT_LEAST マクロが存在しない
#ifndef LIBSNAPPER_VERSION_AT_LEAST
#define LIBSNAPPER_VERSION_AT_LEAST(major, minor)                                            \
    ((LIBSNAPPER_VERSION_MAJOR > (major)) ||                                                 \
     (LIBSNAPPER_VERSION_MAJOR == (major) && LIBSNAPPER_VERSION_MINOR >= (minor)))
#endif

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
#include <snapper/Plugins.h>
#endif

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
static void logPluginReport(const snapper::Plugins::Report& report)
{
    for (const auto& entry : report.entries) {
        if (entry.exit_status != 0) {
            qWarning() << "Snapper plugin" << QString::fromStdString(entry.name)
                       << "exited with status" << entry.exit_status;
        }
    }
}
#endif

/**
 * @brief SnapshotOperationsクラスのコンストラクタ
 *
 * スナップショット操作を管理するクラスを初期化します。
 *
 * @param parent 親QObjectポインタ
 */
SnapshotOperations::SnapshotOperations(QObject *parent)
    : QObject(parent)
    , m_snapper(nullptr)
    , m_currentConfig("")
{
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(IdleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, []() {
        qInfo() << "Idle timeout reached, shutting down...";
        QCoreApplication::quit();
    });
    m_idleTimer.start();
}

/**
 * @brief SnapshotOperationsクラスのデストラクタ
 *
 * リソースのクリーンアップを行います。
 */
SnapshotOperations::~SnapshotOperations()
{
}

/**
 * @brief アイドルタイマーをリセット
 *
 * D-Busメソッド呼び出し時にタイマーをリセットし、
 * アイドルタイムアウトを延長します。
 */
void SnapshotOperations::resetIdleTimer()
{
    m_idleTimer.start();
}

/**
 * @brief D-Busサービスを終了
 *
 * GUIアプリケーションの終了時にD-Bus経由で呼び出され、
 * サービスプロセスを終了させます。
 */
void SnapshotOperations::Quit()
{
    qInfo() << "Quit requested via D-Bus, shutting down...";
    QCoreApplication::quit();
}

/**
 * @brief PolicyKitによる認証チェックを実行
 *
 * 指定されたアクションIDに対してユーザーが権限を持っているかを確認します。
 * 権限がない場合はD-Busエラー応答を送信します。
 *
 * @param actionId チェックするアクションID
 * @return 認証成功時true、失敗時false
 */
bool SnapshotOperations::checkAuthorization(const QString &actionId)
{
    resetIdleTimer();

    PolkitQt1::UnixProcessSubject subject(QDBusConnection::systemBus().interface()->servicePid(message().service()));
    PolkitQt1::Authority::Result result = PolkitQt1::Authority::instance()->checkAuthorizationSync(
        actionId, subject, PolkitQt1::Authority::AllowUserInteraction);

    if (result == PolkitQt1::Authority::Yes) {
        return true;
    }

    sendErrorReply(QDBusError::AccessDenied, "Authorization failed");
    return false;
}

/**
 * @brief Snapperインスタンスを取得
 *
 * 指定された設定名でSnapperインスタンスを取得または作成します。
 * 設定が変更された場合は新しいインスタンスを作成します。
 *
 * @param configName Snapper設定名
 * @return Snapperインスタンスへのポインタ、失敗時はnullptr
 */
snapper::Snapper* SnapshotOperations::getSnapper(const QString &configName)
{
    try {
        // 設定が変更された場合、または初回の場合は新しいSnapperインスタンスを作成
        if (!m_snapper || m_currentConfig != configName) {
            m_snapper.reset(new snapper::Snapper(configName.toStdString(), "/"));
            m_currentConfig = configName;
        }
        return m_snapper.get();
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to create Snapper instance:" << e.what();
        return nullptr;
    }
}

/**
 * @brief スナップショットタイプを文字列に変換
 *
 * snapperライブラリのスナップショットタイプ列挙値を文字列表現に変換します。
 *
 * @param type スナップショットタイプ (snapper::SINGLE, PRE, POST)
 * @return タイプの文字列表現 ("single", "pre", "post")
 */
QString SnapshotOperations::snapshotTypeToString(int type)
{
    switch (type) {
        case snapper::SINGLE: return "single";
        case snapper::PRE: return "pre";
        case snapper::POST: return "post";
        default: return "single"    ;
    }
}

/**
 * @brief 文字列をスナップショットタイプに変換
 *
 * 文字列表現をsnapperライブラリのスナップショットタイプ列挙値に変換します。
 *
 * @param typeStr タイプの文字列表現 ("single", "pre", "post")
 * @return スナップショットタイプ列挙値
 */
int SnapshotOperations::stringToSnapshotType(const QString &typeStr)
{
    if (typeStr == "pre") return snapper::PRE;
    if (typeStr == "post") return snapper::POST;
    return snapper::SINGLE;
}

/**
 * @brief スナップショット一覧をCSV形式に変換
 *
 * Snapperインスタンスから取得したスナップショット一覧を
 * CSV形式の文字列に変換します。
 *
 * @param snapper Snapperインスタンスへのポインタ
 * @return CSV形式のスナップショット情報文字列
 */
QString SnapshotOperations::formatSnapshotToCSV(const snapper::Snapper *snapper)
{
    if (!snapper) {
        return QString();
    }

    QString csv;
    csv += "number,type,pre-number,date,user,cleanup,description,userdata\n";

    const snapper::Snapshots &snapshots = snapper->getSnapshots();
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
        const snapper::Snapshot &snapshot = *it;

        csv += QString::number(snapshot.getNum()) + ",";
        csv += snapshotTypeToString(snapshot.getType()) + ",";
        csv += QString::number(snapshot.getPreNum()) + ",";

        // 日時をISO形式に変換
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(snapshot.getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(snapshot.getUid()) + ",";
        csv += QString::fromStdString(snapshot.getCleanup()) + ",";
        csv += QString::fromStdString(snapshot.getDescription()) + ",";

        // ユーザーデータをkey1=value1,key2=value2形式に変換
        const std::map<std::string, std::string> &userdata = snapshot.getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            userdataPairs.append(QString::fromStdString(pair.first) + "=" +
                               QString::fromStdString(pair.second));
        }
        csv += userdataPairs.join(",");
        csv += "\n";
    }

    return csv;
}

/**
 * @brief スナップショット一覧を取得
 *
 * システム上の全スナップショットをCSV形式で取得します。
 * PolicyKit認証を必要とします。
 *
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::ListSnapshots()
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        return formatSnapshotToCSV(snapper);
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to list snapshots: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 新しいスナップショットを作成
 *
 * 指定されたパラメータで新しいスナップショットを作成します。
 * single、pre、postの3種類のタイプをサポートします。
 *
 * @param type スナップショットのタイプ ("single", "pre", "post")
 * @param description スナップショットの説明
 * @param preNumber postタイプの場合の対応するpreスナップショット番号
 * @param cleanup クリーンアップアルゴリズム名
 * @param important 重要フラグ
 * @return 作成されたスナップショットのCSV情報、失敗時は空文字列
 */
QString SnapshotOperations::CreateSnapshot(const QString &type, const QString &description,
                                          int preNumber, const QString &cleanup, bool important)
{
    if (!checkAuthorization("com.presire.qsnapper.create-snapshot")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::SCD scd;
        scd.description = description.toStdString();
        scd.cleanup = cleanup.toStdString();
        scd.read_only = true;

        if (important) {
            scd.userdata["important"] = "yes";
        }

        snapper::Snapshots::iterator newSnapshot;
        snapper::SnapshotType snapType = static_cast<snapper::SnapshotType>(stringToSnapshotType(type));

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
#endif
        if (snapType == snapper::PRE) {
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createPreSnapshot(scd, report);
#else
            newSnapshot = snapper->createPreSnapshot(scd);
#endif
        }
        else if (snapType == snapper::POST && preNumber > 0) {
            snapper::Snapshots::const_iterator preSnap = snapper->getSnapshots().find(preNumber);
            if (preSnap == snapper->getSnapshots().end()) {
                sendErrorReply(QDBusError::Failed, "Pre-snapshot not found");
                return QString();
            }
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createPostSnapshot(preSnap, scd, report);
#else
            newSnapshot = snapper->createPostSnapshot(preSnap, scd);
#endif
        }
        else {
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            newSnapshot = snapper->createSingleSnapshot(scd, report);
#else
            newSnapshot = snapper->createSingleSnapshot(scd);
#endif
        }
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        logPluginReport(report);
#endif

        // 新しく作成されたスナップショットのCSV情報を返す
        QString csv = "number,type,pre-number,date,user,cleanup,description,userdata\n";
        csv += QString::number(newSnapshot->getNum()) + ",";
        csv += snapshotTypeToString(newSnapshot->getType()) + ",";
        csv += QString::number(newSnapshot->getPreNum()) + ",";

        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(newSnapshot->getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(newSnapshot->getUid()) + ",";
        csv += QString::fromStdString(newSnapshot->getCleanup()) + ",";
        csv += QString::fromStdString(newSnapshot->getDescription()) + ",";

        const std::map<std::string, std::string> &userdata = newSnapshot->getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            userdataPairs.append(QString::fromStdString(pair.first) + "=" +
                               QString::fromStdString(pair.second));
        }
        csv += userdataPairs.join(",");

        return csv;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to create snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to create snapshot: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief スナップショットを削除
 *
 * 指定された番号のスナップショットを削除します。
 * PolicyKit認証を必要とします。
 *
 * @param number 削除するスナップショット番号
 * @return 削除成功時true、失敗時false
 */
bool SnapshotOperations::DeleteSnapshot(int number)
{
    if (!checkAuthorization("com.presire.qsnapper.delete-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapper->deleteSnapshot(snapshot, report);
        logPluginReport(report);
#else
        snapper->deleteSnapshot(snapshot);
#endif
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to delete snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to delete snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief スナップショットにロールバック
 *
 * 指定されたスナップショットをデフォルトに設定し、次回起動時に
 * そのスナップショットの状態で起動するようにします。
 *
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時true、失敗時false
 */
bool SnapshotOperations::RollbackSnapshot(int number)
{
    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショットをデフォルトに設定 (次回起動時に適用される)
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapshot->setDefault(report);
        logPluginReport(report);
#else
        snapshot->setDefault();
#endif
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to rollback snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to rollback snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief ファイル変更一覧を取得
 *
 * 指定されたスナップショットと現在のシステム状態を比較し、
 * 変更されたファイルの一覧を取得します。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::GetFileChanges(const QString &configName, int snapshotNumber)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // snapshot1: 比較元 (指定されたスナップショット)
        // snapshot2: 比較先 (現在のシステム状態)
        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを作成してファイル変更を取得
        // snapshot1からsnapshot2への変更を取得
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, false);
        const snapper::Files &files = comparison.getFiles();

        QString output;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const snapper::File &file = *it;
            unsigned int status = file.getPreToPostStatus();

            // ステータスフラグを文字列に変換
            QString statusStr;
            if (status & snapper::CREATED) statusStr += "+";
            if (status & snapper::DELETED) statusStr += "-";
            if (status & snapper::TYPE) statusStr += "t";
            if (status & snapper::CONTENT) statusStr += "c";
            if (status & snapper::PERMISSIONS) statusStr += "p";
            if (status & snapper::OWNER) statusStr += "u";
            if (status & snapper::GROUP) statusStr += "g";
            if (status & snapper::XATTRS) statusStr += "x";
            if (status & snapper::ACL) statusStr += "a";

            if (statusStr.isEmpty()) statusStr = ".....";

            // パディングして出力フォーマットを整える
            statusStr = statusStr.leftJustified(5, '.');

            output += statusStr + " " + QString::fromStdString(file.getName()) + "\n";
        }

        return output;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}



/**
 * @brief ファイルの差分と詳細情報を一括取得
 *
 * 1回のComparisonオブジェクト生成で、差分(diff)と詳細情報(パーミッション等)の
 * 両方を取得します。GetFileDiff + GetFileDetails の統合版です。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @param filePath 対象ファイルパス
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::GetFileDiffAndDetails(const QString &configName, int snapshotNumber, const QString &filePath)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを1回だけ作成 (スナップショットマウントも1回のみ) 
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, true);
        const snapper::Files &files = comparison.getFiles();

        auto fileIt = files.findAbsolutePath(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

        // --- Details部の構築 ---
        unsigned int status = fileIt->getPreToPostStatus();
        QString statusStr;
        if (status & snapper::CREATED) statusStr += "+";
        if (status & snapper::DELETED) statusStr += "-";
        if (status & snapper::TYPE) statusStr += "t";
        if (status & snapper::CONTENT) statusStr += "c";
        if (status & snapper::PERMISSIONS) statusStr += "p";
        if (status & snapper::OWNER) statusStr += "u";
        if (status & snapper::GROUP) statusStr += "g";
        if (status & snapper::XATTRS) statusStr += "x";
        if (status & snapper::ACL) statusStr += "a";
        if (statusStr.isEmpty()) statusStr = ".....";
        statusStr = statusStr.leftJustified(5, '.');

        auto permsToOctal = [](QFile::Permissions p) -> QString {
            int mode = 0;
            if (p & QFile::ReadOwner)  mode |= 0400;
            if (p & QFile::WriteOwner) mode |= 0200;
            if (p & QFile::ExeOwner)   mode |= 0100;
            if (p & QFile::ReadGroup)  mode |= 0040;
            if (p & QFile::WriteGroup) mode |= 0020;
            if (p & QFile::ExeGroup)   mode |= 0010;
            if (p & QFile::ReadOther)  mode |= 0004;
            if (p & QFile::WriteOther) mode |= 0002;
            if (p & QFile::ExeOther)   mode |= 0001;
            return QString("%1").arg(mode, 4, 8, QChar('0'));
        };

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        QString snapshotPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        QFileInfo snapshotInfo(snapshotPath);
        if (snapshotInfo.exists()) {
            detailsPart += "snapshotPerms=" + permsToOctal(snapshotInfo.permissions()) + "\n";
            detailsPart += "snapshotOwner=" + snapshotInfo.owner() + "\n";
            detailsPart += "snapshotGroup=" + snapshotInfo.group() + "\n";
        }

        QString currentPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_SYSTEM));
        QFileInfo currentInfo(currentPath);
        if (currentInfo.exists()) {
            detailsPart += "currentPerms=" + permsToOctal(currentInfo.permissions()) + "\n";
            detailsPart += "currentOwner=" + currentInfo.owner() + "\n";
            detailsPart += "currentGroup=" + currentInfo.group() + "\n";
        }

        // --- Diff部の取得 ---
        QString diffPart;
        if (snapshotInfo.exists() && currentInfo.exists()) {
            QProcess process;
            process.start("diff", QStringList() << "-u" << snapshotPath << currentPath);
            process.waitForFinished(10000);
            diffPart = QString::fromUtf8(process.readAllStandardOutput());
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff and details:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file diff and details: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルをスナップショットから復元
 *
 * 指定されたファイルリストを指定されたスナップショットの状態に復元します。
 * 復元の進捗はrestoreProgressシグナルで通知されます。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元のスナップショット番号
 * @param filePaths 復元するファイルパスのリスト
 * @return 全ファイルの復元が成功した場合true、それ以外はfalse
 */
bool SnapshotOperations::RestoreFiles(const QString &configName, int snapshotNumber,
                                      const QStringList &filePaths, const QStringList &changeTypes)
{
    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    if (filePaths.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, "No files specified for restore");
        return false;
    }

    if (filePaths.size() != changeTypes.size()) {
        sendErrorReply(QDBusError::InvalidArgs, "filePaths and changeTypes must have the same size");
        return false;
    }

    qWarning() << "RestoreFiles (YaST compatible): Starting restore for" << filePaths.size()
               << "files from snapshot" << snapshotNumber;

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショットをマウント
        snapshot1->mountFilesystemSnapshot(true);

        // スナップショットディレクトリのパスを取得
        QString snapshotDir = QString::fromStdString(snapshot1->snapshotDir());

        qWarning() << "RestoreFiles: Snapshot mounted at" << snapshotDir;

        bool allSuccess = true;
        int total = filePaths.size();
        int successCount = 0;

        for (int i = 0; i < total; ++i) {
            const QString &filePath = filePaths[i];
            const QString &changeType = changeTypes[i];

            // 進捗を通知
            emit restoreProgress(i + 1, total, filePath);

            // スナップショット内のファイルパス
            QString snapshotFilePath = snapshotDir + filePath;
            // システム上のファイルパス
            QString systemFilePath = filePath;

            bool fileSuccess = false;

            if (changeType == "created") {
                // スナップショット時点では存在しなかったファイル → 削除 (YaST: rm -rf)
                QProcess proc;
                proc.start("/bin/rm", QStringList() << "-rf" << "--" << systemFilePath);
                proc.waitForFinished(30000);
                fileSuccess = (proc.exitCode() == 0);
                if (!fileSuccess) {
                    qWarning() << "RestoreFiles: Failed to remove" << systemFilePath;
                }
            }
            else {
                // deleted / modified / typechanged → スナップショットからコピー

                // 親ディレクトリを確認・作成 (YaST: CheckAndCreatePath)
                QString parentDir = systemFilePath.left(systemFilePath.lastIndexOf('/'));
                if (!parentDir.isEmpty()) {
                    QProcess mkdirProc;
                    mkdirProc.start("/bin/mkdir", QStringList() << "-p" << "--" << parentDir);
                    mkdirProc.waitForFinished(10000);
                }

                QFileInfo snapshotFileInfo(snapshotFilePath);
                if (snapshotFileInfo.isDir()) {
                    // ディレクトリの場合 (YaST: mkdir + chown + chmod)
                    if (!QFileInfo::exists(systemFilePath)) {
                        QProcess mkdirProc;
                        mkdirProc.start("/bin/mkdir", QStringList() << "-p" << "--" << systemFilePath);
                        mkdirProc.waitForFinished(10000);
                    }

                    // 所有者をコピー (YaST: /bin/chown -- uid:gid path)
                    QProcess chownProc;
                    chownProc.start("/bin/chown", QStringList()
                        << "--" << QString("%1:%2").arg(snapshotFileInfo.ownerId()).arg(snapshotFileInfo.groupId())
                        << systemFilePath);
                    chownProc.waitForFinished(10000);

                    // パーミッションをコピー (YaST: /bin/chmod -- mode path)
                    QProcess statProc;
                    statProc.start("/usr/bin/stat", QStringList() << "-c" << "%a" << snapshotFilePath);
                    statProc.waitForFinished(10000);
                    QString permStr = QString::fromUtf8(statProc.readAllStandardOutput()).trimmed();
                    if (!permStr.isEmpty()) {
                        QProcess chmodProc;
                        chmodProc.start("/bin/chmod", QStringList() << "--" << permStr << systemFilePath);
                        chmodProc.waitForFinished(10000);
                    }

                    fileSuccess = true;
                }
                else if (snapshotFileInfo.exists()) {
                    // ファイルの場合 (YaST: /bin/cp -a -- src dst)
                    QProcess cpProc;
                    cpProc.start("/bin/cp", QStringList()
                        << "-a" << "--" << snapshotFilePath << systemFilePath);
                    cpProc.waitForFinished(30000);
                    fileSuccess = (cpProc.exitCode() == 0);
                    if (!fileSuccess) {
                        qWarning() << "RestoreFiles: Failed to copy" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }
                else {
                    qWarning() << "RestoreFiles: Source not found in snapshot:" << snapshotFilePath;
                    fileSuccess = false;
                }
            }

            if (fileSuccess) {
                successCount++;
            }
            else {
                allSuccess = false;
            }
        }

        // スナップショットをアンマウント
        try {
            snapshot1->umountFilesystemSnapshot(true);
        }
        catch (...) {
            qWarning() << "RestoreFiles: Failed to unmount snapshot";
        }

        qWarning() << "RestoreFiles: Completed. Successful:" << successCount
                   << "Failed:" << (total - successCount);

        if (!allSuccess) {
            QString errorMsg = QString("Failed to restore %1 out of %2 files").arg(total - successCount).arg(total);
            sendErrorReply(QDBusError::Failed, errorMsg);
        }

        return allSuccess;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "RestoreFiles failed:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to restore files: %1").arg(e.what()));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << "RestoreFiles unexpected error:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Unexpected error: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief ファイルをスナップショットから直接コピーして復元 (高速版)
 *
 * Comparisonオブジェクトを使用せず、スナップショットのマウントパスから
 * 直接ファイルをコピーして復元します。btrfsではreflink (COW) により
 * 高速なコピーが可能です。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元のスナップショット番号
 * @param filePaths 復元するファイルパスのリスト
 * @param changeTypes 各ファイルの変更種別 ("created", "deleted", "modified", "typechanged")
 * @return 全ファイルの復元が成功した場合true、それ以外はfalse
 */
bool SnapshotOperations::RestoreFilesDirect(const QString &configName, int snapshotNumber,
                                            const QStringList &filePaths, const QStringList &changeTypes)
{
    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    if (filePaths.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, "No files specified for restore");
        return false;
    }

    if (filePaths.size() != changeTypes.size()) {
        sendErrorReply(QDBusError::InvalidArgs, "filePaths and changeTypes must have the same size");
        return false;
    }

    qWarning() << "RestoreFilesDirect: Starting direct restore for" << filePaths.size()
               << "files from snapshot" << snapshotNumber;

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショットをマウント
        snapshot1->mountFilesystemSnapshot(true);

        // スナップショットディレクトリのパスを取得
        QString snapshotDir = QString::fromStdString(snapshot1->snapshotDir());

        qWarning() << "RestoreFilesDirect: Snapshot mounted at" << snapshotDir;

        bool allSuccess = true;
        int total = filePaths.size();
        int successCount = 0;

        for (int i = 0; i < total; ++i) {
            const QString &filePath = filePaths[i];
            const QString &changeType = changeTypes[i];

            // 進捗を通知
            emit restoreProgress(i + 1, total, filePath);

            // スナップショット内のファイルパス
            QString snapshotFilePath = snapshotDir + filePath;
            // システム上のファイルパス (ルートからの絶対パス)
            QString systemFilePath = filePath;

            bool fileSuccess = false;

            if (changeType == "created") {
                // スナップショット時点では存在しなかったファイル → 削除
                QProcess proc;
                proc.start("/bin/rm", QStringList() << "-rf" << "--" << systemFilePath);
                proc.waitForFinished(30000);
                fileSuccess = (proc.exitCode() == 0);
                if (!fileSuccess) {
                    qWarning() << "RestoreFilesDirect: Failed to remove" << systemFilePath
                               << "exit code:" << proc.exitCode();
                }
            }
            else {
                // deleted / modified / typechanged → スナップショットからコピー

                // 親ディレクトリを作成
                QString parentDir = systemFilePath.left(systemFilePath.lastIndexOf('/'));
                if (!parentDir.isEmpty()) {
                    QProcess mkdirProc;
                    mkdirProc.start("/bin/mkdir", QStringList() << "-p" << "--" << parentDir);
                    mkdirProc.waitForFinished(10000);
                }

                QFileInfo snapshotFileInfo(snapshotFilePath);
                if (snapshotFileInfo.isDir()) {
                    // ディレクトリの場合: 存在しなければ作成し、権限と所有者をコピー
                    if (!QFileInfo::exists(systemFilePath)) {
                        QProcess mkdirProc;
                        mkdirProc.start("/bin/mkdir", QStringList() << "-p" << "--" << systemFilePath);
                        mkdirProc.waitForFinished(10000);
                    }

                    // 所有者をコピー
                    QProcess chownProc;
                    chownProc.start("/bin/chown", QStringList()
                        << "--" << QString("%1:%2").arg(snapshotFileInfo.ownerId()).arg(snapshotFileInfo.groupId())
                        << systemFilePath);
                    chownProc.waitForFinished(10000);

                    // パーミッションをコピー
                    QProcess chmodProc;
                    QString mode = QString("%1").arg(static_cast<int>(snapshotFileInfo.permissions() & 0x0FFF), 4, 8, QChar('0'));
                    // QFileInfo::permissions()はQFile::Permissionsなので変換
                    QProcess statProc;
                    statProc.start("/usr/bin/stat", QStringList() << "-c" << "%a" << snapshotFilePath);
                    statProc.waitForFinished(10000);
                    QString permStr = QString::fromUtf8(statProc.readAllStandardOutput()).trimmed();
                    if (!permStr.isEmpty()) {
                        chmodProc.start("/bin/chmod", QStringList() << "--" << permStr << systemFilePath);
                        chmodProc.waitForFinished(10000);
                    }

                    fileSuccess = true;
                }
                else if (snapshotFileInfo.exists()) {
                    // ファイル/シンボリックリンクの場合: cp -a --reflink=auto でコピー
                    // 既存ファイルを先に削除 (typechanged対応)
                    if (QFileInfo::exists(systemFilePath)) {
                        QProcess rmProc;
                        rmProc.start("/bin/rm", QStringList() << "-rf" << "--" << systemFilePath);
                        rmProc.waitForFinished(10000);
                    }

                    QProcess cpProc;
                    cpProc.start("/bin/cp", QStringList()
                        << "-a" << "--reflink=auto" << "--" << snapshotFilePath << systemFilePath);
                    cpProc.waitForFinished(30000);
                    fileSuccess = (cpProc.exitCode() == 0);
                    if (!fileSuccess) {
                        qWarning() << "RestoreFilesDirect: Failed to copy" << snapshotFilePath
                                   << "to" << systemFilePath << "exit code:" << cpProc.exitCode();
                    }
                }
                else {
                    qWarning() << "RestoreFilesDirect: Source file not found in snapshot:" << snapshotFilePath;
                    fileSuccess = false;
                }
            }

            if (fileSuccess) {
                successCount++;
            }
            else {
                allSuccess = false;
            }
        }

        // スナップショットをアンマウント
        try {
            snapshot1->umountFilesystemSnapshot(true);
        }
        catch (...) {
            qWarning() << "RestoreFilesDirect: Failed to unmount snapshot";
        }

        qWarning() << "RestoreFilesDirect: Completed. Successful:" << successCount
                   << "Failed:" << (total - successCount);

        if (!allSuccess) {
            QString errorMsg = QString("Failed to restore %1 out of %2 files").arg(total - successCount).arg(total);
            sendErrorReply(QDBusError::Failed, errorMsg);
        }

        return allSuccess;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "RestoreFilesDirect failed:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to restore files: %1").arg(e.what()));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << "RestoreFilesDirect unexpected error:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Unexpected error: %1").arg(e.what()));
        return false;
    }
}
