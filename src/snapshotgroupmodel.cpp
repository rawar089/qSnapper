#include "snapshotgroupmodel.h"
#include "snapshotlistmodel.h"
#include <QHash>

SnapshotGroupModel::SnapshotGroupModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int SnapshotGroupModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_groups.count();
}

QVariant SnapshotGroupModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_groups.count())
        return QVariant();

    const SnapshotGroup &group = m_groups.at(index.row());

    switch (role) {
    case DisplayIdRole:
        if (group.groupType == PrePost)
            return QString("%1 & %2").arg(group.preNumber).arg(group.postNumber);
        return QString::number(group.preNumber);
    case GroupTypeRole:
        return static_cast<int>(group.groupType);
    case GroupTypeStringRole:
        switch (group.groupType) {
        case Single:      return QStringLiteral("single");
        case PrePost:     return QStringLiteral("prepost");
        case UnpairedPre: return QStringLiteral("pre");
        case OrphanPost:  return QStringLiteral("post");
        }
        return QStringLiteral("unknown");
    case StartTimeRole:
        return group.startTime;
    case EndTimeRole:
        return group.endTime;
    case DescriptionRole:
        return group.description;
    case UserRole:
        return group.user;
    case UserdataRole:
        return group.userdata;
    case PreNumberRole:
        return group.preNumber;
    case PostNumberRole:
        return group.postNumber;
    case CleanupAlgoStringRole:
        return group.cleanupAlgoString;
    case IsImportantRole:
        return group.isImportant;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> SnapshotGroupModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[DisplayIdRole] = "displayId";
    roles[GroupTypeRole] = "groupType";
    roles[GroupTypeStringRole] = "groupTypeString";
    roles[StartTimeRole] = "startTime";
    roles[EndTimeRole] = "endTime";
    roles[DescriptionRole] = "description";
    roles[UserRole] = "user";
    roles[UserdataRole] = "userdata";
    roles[PreNumberRole] = "preNumber";
    roles[PostNumberRole] = "postNumber";
    roles[CleanupAlgoStringRole] = "cleanupAlgoString";
    roles[IsImportantRole] = "isImportant";
    return roles;
}

int SnapshotGroupModel::count() const
{
    return m_groups.count();
}

SnapshotListModel *SnapshotGroupModel::sourceModel() const
{
    return m_sourceModel;
}

void SnapshotGroupModel::setSourceModel(SnapshotListModel *model)
{
    if (m_sourceModel == model)
        return;

    if (m_sourceModel) {
        disconnect(m_sourceModel, nullptr, this, nullptr);
    }

    m_sourceModel = model;

    if (m_sourceModel) {
        connect(m_sourceModel, &QAbstractListModel::modelReset,
                this, &SnapshotGroupModel::rebuild);
        connect(m_sourceModel, &QAbstractListModel::rowsInserted,
                this, &SnapshotGroupModel::rebuild);
        connect(m_sourceModel, &QAbstractListModel::rowsRemoved,
                this, &SnapshotGroupModel::rebuild);
        rebuild();
    }

    emit sourceModelChanged();
}

QVariantList SnapshotGroupModel::snapshotNumbersAt(int row) const
{
    if (row < 0 || row >= m_groups.count())
        return {};

    const SnapshotGroup &group = m_groups.at(row);
    QVariantList numbers;
    numbers.append(group.preNumber);
    if (group.postNumber > 0)
        numbers.append(group.postNumber);
    return numbers;
}

void SnapshotGroupModel::rebuild()
{
    if (!m_sourceModel)
        return;

    beginResetModel();
    m_groups.clear();

    const int rowCount = m_sourceModel->rowCount();
    const auto roles = m_sourceModel->roleNames();

    // ロールIDを取得
    int numberRole = -1, typeStringRole = -1, prevNumRole = -1;
    int timestampRole = -1, userRole = -1, descRole = -1;
    int userdataRole = -1, cleanupRole = -1;

    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        if (it.value() == "number") numberRole = it.key();
        else if (it.value() == "snapshotTypeString") typeStringRole = it.key();
        else if (it.value() == "previousNumber") prevNumRole = it.key();
        else if (it.value() == "timestamp") timestampRole = it.key();
        else if (it.value() == "user") userRole = it.key();
        else if (it.value() == "description") descRole = it.key();
        else if (it.value() == "userdata") userdataRole = it.key();
        else if (it.value() == "cleanupAlgoString") cleanupRole = it.key();
    }

    // 全スナップショットのデータを収集
    struct RawSnapshot {
        int number;
        QString typeString;
        int previousNumber;
        QDateTime timestamp;
        QString user;
        QString description;
        QVariantMap userdata;
        QString cleanupAlgoString;
    };

    QList<RawSnapshot> snapshots;
    QHash<int, int> snapshotIndexByNumber;

    for (int i = 0; i < rowCount; ++i) {
        QModelIndex idx = m_sourceModel->index(i, 0);
        RawSnapshot s;
        s.number = m_sourceModel->data(idx, numberRole).toInt();
        s.typeString = m_sourceModel->data(idx, typeStringRole).toString();
        s.previousNumber = m_sourceModel->data(idx, prevNumRole).toInt();
        s.timestamp = m_sourceModel->data(idx, timestampRole).toDateTime();
        s.user = m_sourceModel->data(idx, userRole).toString();
        s.description = m_sourceModel->data(idx, descRole).toString();
        s.userdata = m_sourceModel->data(idx, userdataRole).toMap();
        s.cleanupAlgoString = m_sourceModel->data(idx, cleanupRole).toString();
        snapshots.append(s);
        snapshotIndexByNumber[s.number] = i;
    }

    // Post→Pre のマッピングを構築
    // key: Pre番号, value: Postのインデックス(snapshots配列内)
    QHash<int, int> postIndexByPreNumber;
    for (int i = 0; i < snapshots.count(); ++i) {
        const RawSnapshot &s = snapshots[i];
        if (s.typeString == "post" && s.previousNumber > 0) {
            postIndexByPreNumber[s.previousNumber] = i;
        }
    }

    // 消費済みPostを追跡
    QSet<int> consumedPostIndices;

    for (int i = 0; i < snapshots.count(); ++i) {
        const RawSnapshot &s = snapshots[i];

        if (s.typeString == "single") {
            SnapshotGroup group;
            group.groupType = Single;
            group.preNumber = s.number;
            group.postNumber = 0;
            group.startTime = s.timestamp;
            group.description = s.description;
            group.user = s.user;
            group.userdata = s.userdata;
            group.cleanupAlgoString = s.cleanupAlgoString;
            group.isImportant = s.userdata.value("important").toString() == "yes";
            m_groups.append(group);
        }
        else if (s.typeString == "pre") {
            SnapshotGroup group;
            group.preNumber = s.number;
            group.startTime = s.timestamp;
            group.user = s.user;
            group.cleanupAlgoString = s.cleanupAlgoString;

            if (postIndexByPreNumber.contains(s.number)) {
                int postIdx = postIndexByPreNumber[s.number];
                const RawSnapshot &post = snapshots[postIdx];
                group.groupType = PrePost;
                group.postNumber = post.number;
                group.endTime = post.timestamp;
                group.description = s.description.isEmpty() ? post.description : s.description;
                // userdataを統合 (Pre優先、Postで補完)
                group.userdata = s.userdata;
                for (auto it = post.userdata.constBegin(); it != post.userdata.constEnd(); ++it) {
                    if (!group.userdata.contains(it.key()))
                        group.userdata.insert(it.key(), it.value());
                }
                group.isImportant = s.userdata.value("important").toString() == "yes"
                                 || post.userdata.value("important").toString() == "yes";
                consumedPostIndices.insert(postIdx);
            } else {
                group.groupType = UnpairedPre;
                group.postNumber = 0;
                group.description = s.description;
                group.userdata = s.userdata;
                group.isImportant = s.userdata.value("important").toString() == "yes";
            }
            m_groups.append(group);
        }
        else if (s.typeString == "post") {
            if (!consumedPostIndices.contains(i)) {
                // 孤児Post (対応するPreがない)
                SnapshotGroup group;
                group.groupType = OrphanPost;
                group.preNumber = s.number;
                group.postNumber = 0;
                group.startTime = s.timestamp;
                group.description = s.description;
                group.user = s.user;
                group.userdata = s.userdata;
                group.cleanupAlgoString = s.cleanupAlgoString;
                group.isImportant = s.userdata.value("important").toString() == "yes";
                m_groups.append(group);
            }
        }
    }

    endResetModel();
    emit countChanged();
}
