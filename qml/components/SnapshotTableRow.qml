// スナップショットテーブルの行デリゲート
// SnapshotGroupModelのデータをテーブル行形式で表示
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QSnapper 1.0

ItemDelegate {
    id: root

    // SnapshotGroupModelから自動的に提供されるプロパティ
    required property string displayId
    required property int groupType
    required property string groupTypeString
    required property date startTime
    required property date endTime
    required property string description
    required property string user
    required property var userdata
    required property int preNumber
    required property int postNumber
    required property string cleanupAlgoString
    required property bool isImportant

    property var listModel: null
    property var detailDialog: null
    property var columnWidths: null
    property bool selected: false
    signal selectionToggled()

    height: 40

    contentItem: RowLayout {
        anchors.leftMargin: 5
        anchors.rightMargin: 5
        spacing: 0

        // 選択チェックボックス
        CheckBox {
            checked: root.selected
            onToggled: root.selectionToggled()
            Layout.preferredWidth: 40
            Layout.alignment: Qt.AlignVCenter
        }

        // ID列
        Label {
            text: root.displayId
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.idWidth : 80
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 種類列 (カラーインジケーター付き)
        RowLayout {
            spacing: 6
            Layout.preferredWidth: columnWidths ? columnWidths.typeWidth : 140
            Layout.maximumWidth: Layout.preferredWidth
            Layout.alignment: Qt.AlignVCenter

            // スナップショットタイプを示すカラーバー
            Rectangle {
                width: 4
                height: 22
                radius: 2
                color: {
                    switch (root.groupTypeString) {
                    case "single":
                        return ThemeManager.snapshotTypeSingle
                    case "prepost":
                        return ThemeManager.snapshotTypePost
                    case "pre":
                        return ThemeManager.snapshotTypePre
                    case "post":
                        return ThemeManager.snapshotTypePost
                    default:
                        return ThemeManager.snapshotTypeDefault
                    }
                }
            }

            Label {
                text: {
                    switch (root.groupTypeString) {
                    case "single":
                        return qsTr("Single")
                    case "prepost":
                        return qsTr("Pre and Post")
                    case "pre":
                        return qsTr("Pre")
                    case "post":
                        return qsTr("Post")
                    default:
                        return root.groupTypeString
                    }
                }
                font.pixelSize: 15
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        // 開始日列
        Label {
            text: Qt.formatDateTime(root.startTime, "yyyy-MM-dd HH:mm:ss")
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.dateWidth : 170
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 終了日列
        Label {
            text: root.postNumber > 0 ? Qt.formatDateTime(root.endTime, "yyyy-MM-dd HH:mm:ss") : ""
            font.pixelSize: 15
            Layout.preferredWidth: columnWidths ? columnWidths.dateWidth : 170
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 説明列
        Label {
            text: root.description || qsTr("(No description)")
            font.pixelSize: 15
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // ユーザデータ列
        Label {
            text: {
                if (!root.userdata || Object.keys(root.userdata).length === 0)
                    return ""
                var parts = []
                var keys = Object.keys(root.userdata)
                for (var i = 0; i < keys.length; i++) {
                    parts.push(keys[i] + "=" + root.userdata[keys[i]])
                }
                return parts.join(", ")
            }
            font.pixelSize: 14
            Layout.preferredWidth: columnWidths ? columnWidths.userdataWidth : 150
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 5
        }

        // 詳細ボタン
        Button {
            text: qsTr("Details")
            flat: true
            font.pixelSize: 14
            Layout.preferredWidth: 70
            Layout.alignment: Qt.AlignVCenter
            onClicked: {
                if (root.detailDialog) {
                    root.detailDialog.snapshot = {
                        number: root.preNumber,
                        snapshotType: root.groupType === 1 ? 1 : (root.groupTypeString === "single" ? 0 : 1),
                        snapshotTypeString: root.groupTypeString,
                        previousNumber: 0,
                        timestamp: root.startTime,
                        user: root.user,
                        cleanupAlgo: 0,
                        cleanupAlgoString: root.cleanupAlgoString,
                        description: root.description,
                        userdata: root.userdata
                    }
                    if (root.postNumber > 0) {
                        root.detailDialog.postSnapshot = {
                            number: root.postNumber,
                            timestamp: root.endTime
                        }
                    } else {
                        root.detailDialog.postSnapshot = null
                    }
                    root.detailDialog.open()
                }
            }
        }
    }

    // 行背景
    background: Rectangle {
        color: {
            if (root.selected) {
                return Qt.rgba(palette.highlight.r, palette.highlight.g, palette.highlight.b, 0.25)
            } else if (root.hovered) {
                return palette.midlight
            } else {
                return "transparent"
            }
        }

        // 行区切り線
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: palette.mid
        }
    }
}
