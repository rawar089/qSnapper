#include <QCoreApplication>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <algorithm>
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <snapper/Version.h>
#include <btrfsutil.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utime.h>
#include "snapshotoperations.h"

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

// ============================================================================
// In-process unified diff (Myers diff algorithm)
// QProcess `diff -u` の置き換え
// ============================================================================

namespace {

struct DiffOp {
    enum Type { Equal, Delete, Insert };
    Type type;
    int aIdx, bIdx;  // 0-based index into old/new lines (-1 if N/A)
};

/**
 * Myers diffアルゴリズムで2つの文字列リスト間の最短編集スクリプトを計算する。
 *
 * @param a 旧ファイルの行リスト
 * @param b 新ファイルの行リスト
 * @return 編集操作のリスト (正順)
 */
static QVector<DiffOp> computeMyersDiff(const QStringList &a, const QStringList &b)
{
    const int N = a.size(), M = b.size();

    if (N == 0 && M == 0) return {};
    if (N == 0) {
        QVector<DiffOp> r;
        r.reserve(M);
        for (int i = 0; i < M; i++)
            r.append({DiffOp::Insert, -1, i});
        return r;
    }
    if (M == 0) {
        QVector<DiffOp> r;
        r.reserve(N);
        for (int i = 0; i < N; i++)
            r.append({DiffOp::Delete, i, -1});
        return r;
    }

    const int MAX = N + M, OFF = MAX;

    // V[k + OFF] = 対角線k上の最遠到達x座標
    QVector<int> V(2 * MAX + 1, 0);

    // 各dステップのVスナップショット (バックトラック用)
    QVector<QVector<int>> trace;
    trace.reserve(qMin(MAX, N + M));

    for (int d = 0; d <= MAX; d++) {
        trace.append(V);  // dステップ開始前 (= d-1ステップ終了後) のスナップショット
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && V[OFF + k - 1] < V[OFF + k + 1]))
                    ? V[OFF + k + 1] : V[OFF + k - 1] + 1;
            int y = x - k;
            while (x < N && y < M && a[x] == b[y]) {
                x++; y++;
            }
            V[OFF + k] = x;
            if (x >= N && y >= M) goto done;
        }
    }

done:
    // バックトラックで編集スクリプトを逆順に構築
    {
        QVector<DiffOp::Type> revTypes;
        revTypes.reserve(N + M);
        int x = N, y = M;

        for (int d = trace.size() - 1; d > 0; d--) {
            const QVector<int> &vp = trace[d];  // d-1ステップ終了後のV
            int k = x - y;
            bool down = (k == -d) || (k != d && vp[OFF + k - 1] < vp[OFF + k + 1]);
            int pk = down ? k + 1 : k - 1;
            int px = vp[OFF + pk], py = px - pk;
            int mx = down ? px : px + 1, my = mx - k;

            // 対角線上の等号行 (snake) を逆順に記録
            while (x > mx && y > my) {
                x--; y--;
                revTypes.append(DiffOp::Equal);
            }

            // 非対角移動 (挿入/削除)
            revTypes.append(down ? DiffOp::Insert : DiffOp::Delete);
            x = px; y = py;
        }

        // d=0の初期 snake (等号行のみ、編集なし)
        while (x > 0 && y > 0) {
            x--; y--;
            revTypes.append(DiffOp::Equal);
        }

        // 正順に反転
        std::reverse(revTypes.begin(), revTypes.end());

        // 操作タイプからインデックス付きDiffOpに変換
        QVector<DiffOp> result;
        result.reserve(revTypes.size());
        int ai = 0, bi = 0;
        for (auto t : revTypes) {
            switch (t) {
                case DiffOp::Equal:
                    result.append({DiffOp::Equal, ai, bi}); ai++; bi++; break;
                case DiffOp::Delete:
                    result.append({DiffOp::Delete, ai, -1}); ai++; break;
                case DiffOp::Insert:
                    result.append({DiffOp::Insert, -1, bi}); bi++; break;
            }
        }
        return result;
    }
}

/**
 * 2つのファイルを読み込み、unified diff形式の文字列を生成する。
 * `diff -u` と互換性のあるフォーマットで、QMLのformatDiffHtml()でパース可能。
 *
 * @param oldPath 旧ファイルパス (--- ヘッダに使用)
 * @param newPath 新ファイルパス (+++ ヘッダに使用)
 * @return unified diff文字列、差分がない場合は空文字列
 */
static QString generateUnifiedDiff(const QString &oldPath, const QString &newPath)
{
    QFile oldFile(oldPath), newFile(newPath);
    if (!oldFile.open(QIODevice::ReadOnly | QIODevice::Text) ||
        !newFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList a = QString::fromUtf8(oldFile.readAll()).split('\n');
    QStringList b = QString::fromUtf8(newFile.readAll()).split('\n');
    oldFile.close();
    newFile.close();

    // ファイル末尾の改行で生じる空要素を除去
    if (!a.isEmpty() && a.last().isEmpty()) a.removeLast();
    if (!b.isEmpty() && b.last().isEmpty()) b.removeLast();

    QVector<DiffOp> ops = computeMyersDiff(a, b);

    // 変更がない場合は空文字列を返す (diff -u の差分なしと同じ挙動)
    bool hasChanges = false;
    for (const auto &op : ops) {
        if (op.type != DiffOp::Equal) { hasChanges = true; break; }
    }
    if (!hasChanges) return {};

    // 変更位置を特定
    const int context = 3;
    QVector<int> changes;
    for (int i = 0; i < ops.size(); i++) {
        if (ops[i].type != DiffOp::Equal) changes.append(i);
    }

    // hunkにグループ化 (距離が 2*context 以内の変更をマージ)
    struct Hunk { int start, end; };
    QVector<Hunk> hunks;
    int hs = changes[0], he = changes[0];
    for (int i = 1; i < changes.size(); i++) {
        if (changes[i] - he <= 2 * context)
            he = changes[i];
        else {
            hunks.append({hs, he});
            hs = he = changes[i];
        }
    }
    hunks.append({hs, he});

    // unified diff形式で出力
    QString out;
    out += "--- " + oldPath + "\n";
    out += "+++ " + newPath + "\n";

    for (const auto &h : hunks) {
        int s = qMax(0, h.start - context);
        int e = qMin(ops.size() - 1, h.end + context);

        // hunk前の行数をカウント (行番号計算用)
        int aBefore = 0, bBefore = 0;
        for (int i = 0; i < s; i++) {
            if (ops[i].type != DiffOp::Insert) aBefore++;
            if (ops[i].type != DiffOp::Delete) bBefore++;
        }

        // hunk内の行数をカウント
        int aCount = 0, bCount = 0;
        for (int i = s; i <= e; i++) {
            if (ops[i].type != DiffOp::Insert) aCount++;
            if (ops[i].type != DiffOp::Delete) bCount++;
        }

        // 行番号は1ベース、空hunkの場合は0
        out += QString("@@ -%1,%2 +%3,%4 @@\n")
            .arg(aCount == 0 ? 0 : aBefore + 1).arg(aCount)
            .arg(bCount == 0 ? 0 : bBefore + 1).arg(bCount);

        for (int i = s; i <= e; i++) {
            switch (ops[i].type) {
                case DiffOp::Equal:
                    out += " " + a[ops[i].aIdx] + "\n";
                    break;
                case DiffOp::Delete:
                    out += "-" + a[ops[i].aIdx] + "\n";
                    break;
                case DiffOp::Insert:
                    out += "+" + b[ops[i].bIdx] + "\n";
                    break;
            }
        }
    }

    return out;
}

} // anonymous namespace

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
    , m_authenticated(false)
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
/**
 * @brief Snapperが設定されているか確認
 *
 * Snapper設定が1つ以上存在するかを確認します。
 * 認証は不要 (list-snapshotsと同じアクションでactiveユーザーは自動許可)。
 *
 * @return Snapper設定が存在する場合true
 */
bool SnapshotOperations::IsConfigured()
{
    try {
        std::list<snapper::ConfigInfo> configList = snapper::Snapper::getConfigs("/");
        return !configList.empty();
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to check if snapper is configured:" << e.what();
        return false;
    }
}

/**
 * @brief Snapper設定を書き込む
 *
 * 指定されたキー/バリューペアをSnapper設定に書き込みます。
 * PolicyKit認証を必要とします。
 *
 * @param configName Snapper設定名
 * @param settings 設定のキー/バリューマップ
 * @return 成功時true、失敗時false
 */
bool SnapshotOperations::WriteSnapperConfig(const QString &configName,
                                            const QMap<QString, QString> &settings)
{
    if (!checkAuthorization("com.presire.qsnapper.configure")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        std::map<std::string, std::string> info;
        for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
            info[it.key().toStdString()] = it.value().toStdString();
        }

        snapper->setConfigInfo(info);
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to write snapper config:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to write config: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief Snapperのクォータを設定
 *
 * 指定されたSnapper設定のクォータ機能を設定します。
 * PolicyKit認証を必要とします。
 *
 * @param configName Snapper設定名
 * @return 成功時true、失敗時false
 */
bool SnapshotOperations::SetupQuota(const QString &configName)
{
    if (!checkAuthorization("com.presire.qsnapper.configure")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper->setupQuota();
        return true;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to setup quota:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to setup quota: %1").arg(e.what()));
        return false;
    }
}

void SnapshotOperations::Quit()
{
    qInfo() << "Quit requested via D-Bus, shutting down...";
    QCoreApplication::quit();
}

/**
 * @brief 事前認証を実行
 *
 * バッチ復元などの連続操作の前に1回だけ呼び出し、Polkit認証を実行します。
 * 認証成功時にm_authenticatedフラグをセットし、以降のRestoreFiles/RestoreFilesDirect呼び出しでは再認証をスキップします。
 * フラグはD-Busサービスプロセスの生存期間中有効です。 (アイドルタイムアウト5分で自動クリア)
 *
 * @param actionId チェックするPolkitアクションID
 * @return 認証成功時true、失敗時false
 */
bool SnapshotOperations::Authenticate(const QString &actionId)
{
    m_authenticated = false;
    bool result = checkAuthorization(actionId);
    if (result) {
        m_authenticated = true;
    }
    return result;
}

/**
 * @brief PolicyKitによる認証チェックを実行
 *
 * 指定されたアクションIDに対してユーザが権限を持っているかを確認します。
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
snapper::Snapper* SnapshotOperations::getSnapper(const QString &configName, bool forceReload)
{
    try {
        // 設定変更時・初回・強制リロード指定時に新しいSnapperインスタンスを作成。
        // libsnapper の Snapper オブジェクトは構築時にスナップショット一覧を
        // 読み込み、外部で作成された新規スナップショットを自動で取り込まないため、
        // 一覧更新時には forceReload でインスタンスを作り直す必要がある。
        if (!m_snapper || m_currentConfig != configName || forceReload) {
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
 * Snapperインスタンスから取得したスナップショット一覧をCSV形式の文字列に変換します。
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

        // ユーザデータを key1=value1,key2=value2形式に変換
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
/**
 * @brief 利用可能な Snapper 設定名のリストを返す
 *
 * "snapper --no-dbus --csvout list-configs --columns config"を呼び出し、
 * 設定名のみを抜き出して配列で返します。
 */
QStringList SnapshotOperations::ListConfigs()
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QStringList();
    }

    try {
        std::list<snapper::ConfigInfo> configList = snapper::Snapper::getConfigs("/");
        QStringList configs;
        for (const auto &ci : configList) {
#if LIBSNAPPER_VERSION_AT_LEAST(6, 0)
            configs.append(QString::fromStdString(ci.get_config_name()));
#else
            configs.append(QString::fromStdString(ci.getConfigName()));
#endif
        }
        return configs;
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapper configs:" << e.what();
        return QStringList();
    }
}

QString SnapshotOperations::ListSnapshots(const QString &configName)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        // 一覧取得時は必ず再構築して外部で作成された最新スナップショットを反映する
        snapper::Snapper *snapper = getSnapper(
            configName.isEmpty() ? QStringLiteral("root") : configName,
            /*forceReload=*/true);
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
QString SnapshotOperations::CreateSnapshot(const QString &configName, const QString &type, const QString &description,
                                          int preNumber, const QString &cleanup,
                                          const QMap<QString, QString> &userdata, bool important)
{
    if (!checkAuthorization("com.presire.qsnapper.create-snapshot")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::SCD scd;
        scd.description = description.toStdString();
        scd.cleanup = cleanup.toStdString();
        scd.read_only = true;

        // ユーザが指定した key=value 形式のユーザデータをコピー
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            scd.userdata[it.key().toStdString()] = it.value().toStdString();
        }

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
/**
 * @brief 既存スナップショットのメタデータを編集
 *
 * description / cleanup algorithm / userdata を差し替えます。
 * 空文字列("")のdescriptionはそのまま空文字列で上書きされます。
 * userdataは渡されたマップで完全に置き換わります。(差分ではない)
 */
bool SnapshotOperations::ModifySnapshot(const QString &configName, int number,
                                        const QString &description, const QString &cleanup,
                                        const QMap<QString, QString> &userdata)
{
    if (!checkAuthorization("com.presire.qsnapper.modify-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        snapper::SMD smd;
        smd.description = description.toStdString();
        smd.cleanup     = cleanup.toStdString();
        for (auto it = userdata.constBegin(); it != userdata.constEnd(); ++it) {
            smd.userdata[it.key().toStdString()] = it.value().toStdString();
        }

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
        snapper->modifySnapshot(snapshot, smd, report);
        logPluginReport(report);
#else
        snapper->modifySnapshot(snapshot, smd);
#endif
        return true;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to modify snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to modify snapshot: %1").arg(e.what()));
        return false;
    }
}

bool SnapshotOperations::DeleteSnapshot(const QString &configName, int number)
{
    // 事前認証済み(Authenticate呼び出し済み)の場合はスキップ、未認証なら従来通り認証
    if (!m_authenticated && !checkAuthorization("com.presire.qsnapper.delete-snapshot")) {
        return false;
    }
    resetIdleTimer();

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
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
        resetIdleTimer();   // 長時間削除後もタイマーリセット
        return true;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to delete snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to delete snapshot: %1").arg(e.what()));
        resetIdleTimer();   // 例外時もタイマーリセット
        return false;
    }
}

/**
 * @brief スナップショットにロールバック
 *
 * 指定されたスナップショットをデフォルトに設定し、次回起動時にそのスナップショットの状態で起動するようにします。
 *
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時true、失敗時false
 */
bool SnapshotOperations::RollbackSnapshot(const QString &configName, int number)
{
    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots &snapshots = snapper->getSnapshots();
        snapper::Snapshots::iterator target = snapshots.find(number);
        if (target == snapshots.end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // 'snapper rollback N' と同等の挙動を再現する。
        //
        // CLI (client/snapper/cmd-rollback.cc) はambitを以下で判定する:
        //   - previous_defaultがread-only --> TRANSACTIONAL
        //     (新規スナップショット作成なしで対象を直接default化)
        //   - previous_defaultがwritable --> CLASSIC
        //       (1) 現在状態のread-onlyバックアップsnapshotを作成
        //       (2) 対象Nのwritable copy snapshotを作成
        //       (3) previous_defaultにcleanupが空なら"number"を付与
        //       (4) (2)で作成したwritable copyをdefaultに設定
        snapper::Snapshots::iterator previousDefault = snapshots.getDefault();
        const bool transactional =
            (previousDefault != snapshots.end() && previousDefault->isReadOnly());

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
        snapper::Plugins::Report report;
#endif

        if (transactional) {
            // TRANSACTIONAL: 対象スナップショットをそのままdefaultに
#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            target->setDefault(report);
            logPluginReport(report);
#else
            target->setDefault();
#endif
        } else {
            // CLASSIC: backup + writable copyを作ってwritable copyをdefaultに

            const int prevNum =
                (previousDefault != snapshots.end()) ? static_cast<int>(previousDefault->getNum()) : -1;

            // (1) 現在状態のread-onlyバックアップ
            snapper::SCD scd1;
            scd1.description = (prevNum >= 0)
                ? std::string("rollback backup of #") + std::to_string(prevNum)
                : std::string("rollback backup");
            scd1.cleanup = "number";
            scd1.userdata["important"] = "yes";
            scd1.read_only = true;

            // (2) 対象Nのwritable copy
            snapper::SCD scd2;
            scd2.description = std::string("writable copy of #") + std::to_string(number);
            scd2.cleanup.clear();
            scd2.read_only = false;

#if LIBSNAPPER_VERSION_AT_LEAST(7, 4)
            snapper::Snapshots::iterator backup =
                snapper->createSingleSnapshot(scd1, report);
            logPluginReport(report);

            snapper::Snapshots::iterator writableCopy =
                snapper->createSingleSnapshot(target, scd2, report);
            logPluginReport(report);

            // (3) previous_defaultにcleanupが空なら"number"を付与
            if (previousDefault != snapshots.end() && previousDefault->getCleanup().empty()) {
                snapper::SMD smd;
                smd.description = previousDefault->getDescription();
                smd.cleanup     = "number";
                smd.userdata    = previousDefault->getUserdata();
                snapper->modifySnapshot(previousDefault, smd, report);
                logPluginReport(report);
            }

            // (4) writable copyをdefaultに
            writableCopy->setDefault(report);
            logPluginReport(report);
#else
            snapper::Snapshots::iterator backup =
                snapper->createSingleSnapshot(scd1);

            snapper::Snapshots::iterator writableCopy =
                snapper->createSingleSnapshot(target, scd2);

            if (previousDefault != snapshots.end() && previousDefault->getCleanup().empty()) {
                snapper::SMD smd;
                smd.description = previousDefault->getDescription();
                smd.cleanup     = "number";
                smd.userdata    = previousDefault->getUserdata();
                snapper->modifySnapshot(previousDefault, smd);
            }

            writableCopy->setDefault();
#endif
            (void)backup;
        }

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
 * 指定されたスナップショットと現在のシステム状態を比較し、変更されたファイルの一覧を取得します。
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
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
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
 * @brief 2 つのスナップショット間のファイル変更リストを取得
 *
 * snapshot1 → snapshot2の差分を取得します。現在のシステム状態は使用しません。
 * GetFileChangesの「任意の2つのsnapshot間」版です。
 */
QString SnapshotOperations::GetFileChangesBetween(const QString &configName, int number1, int number2)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        snapper::Comparison comparison(snapper, snapshot1, snapshot2, false);
        const snapper::Files &files = comparison.getFiles();

        QString output;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const snapper::File &file = *it;
            unsigned int status = file.getPreToPostStatus();

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

            output += statusStr + " " + QString::fromStdString(file.getName()) + "\n";
        }

        return output;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes between snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 2つのスナップショット間の個別ファイルの詳細 + diff を取得
 *
 * GetFileDiffAndDetailsの任意2つのsnapshot間版
 * snapshot1側のパーミッションとsnapshot2側のパーミッションを返し、diff部も両snapshot上のファイルを比較します。
 */
QString SnapshotOperations::GetFileDiffBetween(const QString &configName, int number1, int number2, const QString &filePath)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        snapper::Comparison comparison(snapper, snapshot1, snapshot2, true);
        const snapper::Files &files = comparison.getFiles();

        auto fileIt = files.findAbsolutePath(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

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

        // snapshot1をLOC_PREとして扱い、snapshot2をLOC_POSTとして扱う
        QString path1 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        QFileInfo info1(path1);
        if (info1.exists()) {
            detailsPart += "snapshotPerms=" + permsToOctal(info1.permissions()) + "\n";
            detailsPart += "snapshotOwner=" + info1.owner() + "\n";
            detailsPart += "snapshotGroup=" + info1.group() + "\n";
        }

        QString path2 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_POST));
        QFileInfo info2(path2);
        if (info2.exists()) {
            detailsPart += "currentPerms=" + permsToOctal(info2.permissions()) + "\n";
            detailsPart += "currentOwner=" + info2.owner() + "\n";
            detailsPart += "currentGroup=" + info2.group() + "\n";
        }

        QString diffPart;
        if (info1.exists() && info2.exists()) {
            diffPart = generateUnifiedDiff(path1, path2);
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff between snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file diff: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルの差分と詳細情報を一括取得
 *
 * 1回のComparisonオブジェクト生成で、差分(diff)と詳細情報(パーミッション等)の両方を取得します。
 * GetFileDiff + GetFileDetailsの統合版。
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
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
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
            diffPart = generateUnifiedDiff(snapshotPath, currentPath);
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
    resetIdleTimer();
    // 事前認証済み(Authenticate呼び出し済み)の場合はスキップ、未認証なら従来通り認証
    if (!m_authenticated && !checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
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
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
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
        int skippedCount = 0;

        for (int i = 0; i < total; ++i) {
            const QString &filePath = filePaths[i];
            const QString &changeType = changeTypes[i];

            // 進捗を通知
            emit restoreProgress(i + 1, total, filePath);

            // 危険なパスをスキップ
            if (filePath.startsWith("/.snapshots/")) {
                qWarning() << "RestoreFiles: Skipping dangerous path:" << filePath;
                skippedCount++;
                continue;
            }

            // スナップショット内のファイルパス
            QString snapshotFilePath = snapshotDir + filePath;
            // システム上のファイルパス
            QString systemFilePath = filePath;

            bool fileSuccess = false;

            if (changeType == "created") {
                // スナップショット時点では存在しなかったファイル → 削除
                std::error_code ec;
                std::filesystem::remove_all(systemFilePath.toStdString(), ec);
                fileSuccess = !ec;
                if (!fileSuccess) {
                    qWarning() << "RestoreFiles: Failed to remove" << systemFilePath << ec.message().c_str();
                }
            }
            else {
                // deleted / modified / typechanged → スナップショットからコピー

                // 親ディレクトリを確認・作成
                QString parentDir = systemFilePath.left(systemFilePath.lastIndexOf('/'));
                if (!parentDir.isEmpty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(parentDir.toStdString(), ec);
                }

                QFileInfo snapshotFileInfo(snapshotFilePath);
                if (snapshotFileInfo.isSymLink()) {
                    // シンボリックリンクの場合
                    fileSuccess = copySymlink(snapshotFilePath, systemFilePath);
                    if (!fileSuccess) {
                        qWarning() << "RestoreFiles: Failed to copy symlink" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }
                else if (snapshotFileInfo.isDir()) {
                    // ディレクトリの場合: 作成 + chown + chmod
                    if (!QFileInfo::exists(systemFilePath)) {
                        std::error_code ec;
                        std::filesystem::create_directories(systemFilePath.toStdString(), ec);
                    }

                    // 所有者をコピー
                    chown(systemFilePath.toUtf8().constData(),
                          snapshotFileInfo.ownerId(), snapshotFileInfo.groupId());

                    // パーミッションをコピー (POSIX stat + chmod)
                    struct stat st;
                    if (lstat(snapshotFilePath.toUtf8().constData(), &st) == 0) {
                        chmod(systemFilePath.toUtf8().constData(), st.st_mode);
                    }

                    fileSuccess = true;
                }
                else if (snapshotFileInfo.exists()) {
                    // 通常ファイルの場合
                    fileSuccess = copyRegularFile(snapshotFilePath, systemFilePath, false);
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

        // 安全ネット: 復元操作によりルートサブボリュームがread-onlyになっていないか確認・復旧
        {
            bool isReadOnly = false;
            if (btrfs_util_get_subvolume_read_only("/", &isReadOnly) == BTRFS_UTIL_OK && isReadOnly) {
                qWarning() << "RestoreFiles: Root subvolume became read-only after restore, restoring rw";
                btrfs_util_set_subvolume_read_only("/", false);
            }
        }

        // スナップショットをアンマウント
        try {
            snapshot1->umountFilesystemSnapshot(true);
        }
        catch (...) {
            qWarning() << "RestoreFiles: Failed to unmount snapshot";
        }

        if (skippedCount > 0) {
            qWarning() << "RestoreFiles: Skipped" << skippedCount << "dangerous paths";
        }
        qWarning() << "RestoreFiles: Completed. Successful:" << successCount
                   << "Failed:" << (total - successCount - skippedCount);

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
 * Comparisonオブジェクトを使用せず、スナップショットのマウントパスから直接ファイルをコピーして復元します。
 * btrfsではreflink (COW) により高速なコピーが可能です。
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
    resetIdleTimer();
    // 事前認証済み(Authenticate呼び出し済み)の場合はスキップ、未認証なら従来通り認証
    if (!m_authenticated && !checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
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
        snapper::Snapper *snapper = getSnapper(configName.isEmpty() ? QStringLiteral("root") : configName);
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
        int skippedCount = 0;

        for (int i = 0; i < total; ++i) {
            const QString &filePath = filePaths[i];
            const QString &changeType = changeTypes[i];

            // 進捗を通知
            emit restoreProgress(i + 1, total, filePath);

            // 危険なパスをスキップ
            if (filePath.startsWith("/.snapshots/")) {
                qWarning() << "RestoreFilesDirect: Skipping dangerous path:" << filePath;
                skippedCount++;
                continue;
            }

            // スナップショット内のファイルパス
            QString snapshotFilePath = snapshotDir + filePath;
            // システム上のファイルパス (ルートからの絶対パス)
            QString systemFilePath = filePath;

            bool fileSuccess = false;

            if (changeType == "created") {
                // スナップショット時点では存在しなかったファイル → 削除
                std::error_code ec;
                std::filesystem::remove_all(systemFilePath.toStdString(), ec);
                fileSuccess = !ec;
                if (!fileSuccess) {
                    qWarning() << "RestoreFilesDirect: Failed to remove" << systemFilePath
                               << ec.message().c_str();
                }
            }
            else {
                // deleted / modified / typechanged → スナップショットからコピー

                // 親ディレクトリを作成
                QString parentDir = systemFilePath.left(systemFilePath.lastIndexOf('/'));
                if (!parentDir.isEmpty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(parentDir.toStdString(), ec);
                }

                QFileInfo snapshotFileInfo(snapshotFilePath);
                if (snapshotFileInfo.isSymLink()) {
                    // シンボリックリンクの場合
                    fileSuccess = copySymlink(snapshotFilePath, systemFilePath);
                    if (!fileSuccess) {
                        qWarning() << "RestoreFilesDirect: Failed to copy symlink" << snapshotFilePath
                                   << "to" << systemFilePath;
                    }
                }
                else if (snapshotFileInfo.isDir()) {
                    // ディレクトリの場合: 作成 + chown + chmod
                    if (!QFileInfo::exists(systemFilePath)) {
                        std::error_code ec;
                        std::filesystem::create_directories(systemFilePath.toStdString(), ec);
                    }

                    // 所有者をコピー
                    chown(systemFilePath.toUtf8().constData(),
                          snapshotFileInfo.ownerId(), snapshotFileInfo.groupId());

                    // パーミッションをコピー
                    struct stat st;
                    if (lstat(snapshotFilePath.toUtf8().constData(), &st) == 0) {
                        chmod(systemFilePath.toUtf8().constData(), st.st_mode);
                    }

                    fileSuccess = true;
                }
                else if (snapshotFileInfo.exists()) {
                    // typechanged の場合のみ既存ファイルを先に削除
                    if (changeType == "typechanged" && QFileInfo::exists(systemFilePath)) {
                        std::error_code ec;
                        std::filesystem::remove_all(systemFilePath.toStdString(), ec);
                        if (ec) {
                            qWarning() << "RestoreFilesDirect: Failed to remove before copy"
                                       << systemFilePath << ec.message().c_str();
                            allSuccess = false;
                            continue;
                        }
                    }

                    // ファイルの場合: reflink (btrfs CoW) を試行
                    fileSuccess = copyRegularFile(snapshotFilePath, systemFilePath, true);
                    if (!fileSuccess) {
                        qWarning() << "RestoreFilesDirect: Failed to copy" << snapshotFilePath
                                   << "to" << systemFilePath;
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

        // 安全ネット: 復元操作によりルートサブボリュームがread-onlyになっていないか確認・復旧
        {
            bool isReadOnly = false;
            if (btrfs_util_get_subvolume_read_only("/", &isReadOnly) == BTRFS_UTIL_OK && isReadOnly) {
                qWarning() << "RestoreFilesDirect: Root subvolume became read-only after restore, restoring rw";
                btrfs_util_set_subvolume_read_only("/", false);
            }
        }

        // スナップショットをアンマウント
        try {
            snapshot1->umountFilesystemSnapshot(true);
        }
        catch (...) {
            qWarning() << "RestoreFilesDirect: Failed to unmount snapshot";
        }

        if (skippedCount > 0) {
            qWarning() << "RestoreFilesDirect: Skipped" << skippedCount << "dangerous paths";
        }
        qWarning() << "RestoreFilesDirect: Completed. Successful:" << successCount
                   << "Failed:" << (total - successCount - skippedCount);

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

/**
 * @brief 通常ファイルをコピー (sendfile + 権限・所有者・タイムスタンプ保持)
 *
 * tryReflink=trueの場合、まずioctl(FICLONE)を試行し、
 * btrfs CoW (reflink)が使用可能であれば高速コピー、失敗時はsendfileにフォールバック。
 *
 * "cp -d --preserve=all --no-preserve=xattr"と同等の動作。
 */
bool SnapshotOperations::copyRegularFile(const QString &src, const QString &dst, bool tryReflink)
{
    int srcFd = open(src.toUtf8().constData(), O_RDONLY);
    if (srcFd < 0) {
        qWarning() << "copyRegularFile: Failed to open source:" << src << strerror(errno);
        return false;
    }

    struct stat srcStat;
    if (fstat(srcFd, &srcStat) < 0) {
        qWarning() << "copyRegularFile: Failed to stat source:" << src << strerror(errno);
        close(srcFd);
        return false;
    }

    int dstFd = open(dst.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC, srcStat.st_mode);
    if (dstFd < 0) {
        qWarning() << "copyRegularFile: Failed to open destination:" << dst << strerror(errno);
        close(srcFd);
        return false;
    }

    bool copied = false;

    // Step 1: reflink (btrfs CoW)を試行
    if (tryReflink) {
        if (ioctl(dstFd, FICLONE, srcFd) == 0) {
            copied = true;
        }
        // FICLONE 失敗時は sendfile にフォールバック
    }

    // Step 2: sendfileでデータコピー
    if (!copied) {
        off_t offset = 0;
        ssize_t remaining = srcStat.st_size;
        while (remaining > 0) {
            ssize_t written = sendfile(dstFd, srcFd, &offset, remaining);
            if (written < 0) {
                qWarning() << "copyRegularFile: sendfile failed:" << strerror(errno);
                close(dstFd);
                close(srcFd);
                return false;
            }
            remaining -= written;
        }
    }

    // 所有者を保持 (cp --preserve=all)
    if (fchown(dstFd, srcStat.st_uid, srcStat.st_gid) < 0) {
        // root権限でのみ成功する; 失敗は警告のみ
        qWarning() << "copyRegularFile: fchown failed (non-fatal):" << strerror(errno);
    }

    // タイムスタンプを保持
    struct timespec ts[2];
    ts[0] = srcStat.st_atim;
    ts[1] = srcStat.st_mtim;
    futimens(dstFd, ts);

    close(dstFd);
    close(srcFd);
    return true;
}

/**
 * @brief シンボリックリンクをコピー (readlink → symlink + lchown + タイムスタンプ)
 *
 * "cp -d --preserve=all --no-preserve=xattr"のシンボリックリンク版。
 */
bool SnapshotOperations::copySymlink(const QString &src, const QString &dst)
{
    char buf[PATH_MAX];
    ssize_t len = readlink(src.toUtf8().constData(), buf, sizeof(buf) - 1);
    if (len < 0) {
        qWarning() << "copySymlink: readlink failed:" << src << strerror(errno);
        return false;
    }
    buf[len] = '\0';

    // 既存ファイル/リンクを先に削除
    std::filesystem::remove(std::filesystem::path(dst.toStdString()));

    if (symlink(buf, dst.toUtf8().constData()) < 0) {
        qWarning() << "copySymlink: symlink failed:" << dst << strerror(errno);
        return false;
    }

    // 所有者を保持 (lchown = リンク自体の所有者を変更)
    struct stat srcStat;
    if (lstat(src.toUtf8().constData(), &srcStat) == 0) {
        lchown(dst.toUtf8().constData(), srcStat.st_uid, srcStat.st_gid);
    }

    // タイムスタンプを保持 (l utimes 相当)
    struct timespec ts[2];
    ts[0] = srcStat.st_atim;
    ts[1] = srcStat.st_mtim;
    utimensat(AT_FDCWD, dst.toUtf8().constData(), ts, AT_SYMLINK_NOFOLLOW);

    return true;
}
