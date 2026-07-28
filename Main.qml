import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import "ProviderCatalog.js" as Catalog

ApplicationWindow {
    id: window

    width: 1240
    height: 780
    minimumWidth: 960
    minimumHeight: 620
    visible: true
    color: "#0b0e14"
    title: qsTr("SVM · SDK Version Manager")

    property int selectedSdk: 0
    property string currentSection: "download"
    property string searchText: ""
    property string versionSearchText: ""
    property string versionFilter: "all"
    property string toastText: ""
    property string pendingDeleteProvider: ""
    property string pendingDeleteVersion: ""
    property string pendingDeleteName: ""
    property var downloadedVersions: ({})
    property var sdkCatalog: Catalog.providersFor(currentSection)
    property var filteredCatalog: filterProviders(sdkCatalog, searchText)
    property var sectionInfo: Catalog.section(currentSection)
    property var displayedVersions: providerController.activeProvider === selectedItem().key
                                    && providerController.versions.length > 0
                                    ? providerController.versions : selectedItem().versions
    property var filteredVersions: filterVersions(displayedVersions, versionSearchText, versionFilter)

    Component.onCompleted: {
        if (currentSection === "download")
            providerController.loadVersions(selectedItem().key)
    }

    function hasVersionProvider(providerKey) {
        return providerKey === "flutter" || providerKey === "node" || providerKey === "java"
    }

    function filterProviders(source, query) {
        var normalized = String(query || "").trim().toLowerCase()
        if (normalized.length === 0)
            return source

        var result = []
        for (var i = 0; i < source.length; ++i) {
            var item = source[i]
            var searchable = (item.name + " " + item.key + " " + item.summary).toLowerCase()
            if (searchable.indexOf(normalized) >= 0)
                result.push(item)
        }
        return result
    }

    function selectProvider(providerKey) {
        for (var i = 0; i < sdkCatalog.length; ++i) {
            if (sdkCatalog[i].key === providerKey) {
                selectedSdk = i
                versionSearchText = ""
                versionFilter = "all"
                versionSearchField.clear()
                providerController.loadVersions(providerKey)
                return
            }
        }
    }

    function selectSection(sectionKey) {
        currentSection = sectionKey
        selectedSdk = 0
        searchText = ""
        versionSearchText = ""
        versionFilter = "all"
        searchField.clear()
        versionSearchField.clear()
        if (sectionKey === "download") {
            Qt.callLater(function() {
                providerController.loadVersions(selectedItem().key)
            })
        }
    }

    function filterVersions(source, query, channelFilter) {
        var normalized = String(query || "").trim().toLowerCase()
        var result = []
        var providerKey = selectedItem().key
        for (var i = 0; i < source.length; ++i) {
            var item = source[i]
            var channel = String(item.channel || "").toLowerCase()
            var matchesText = normalized.length === 0
                    || String(item.version).toLowerCase().indexOf(normalized) >= 0
            var matchesChannel = channelFilter === "all"
                    || (channelFilter === "lts" && channel.indexOf("lts") >= 0)
                    || (channelFilter === "current" && channel.indexOf("current") >= 0)
                    || (channelFilter === "stable" && channel === "stable")
                    || (channelFilter === "beta" && channel === "beta")
            if (matchesText && matchesChannel)
                result.push(item)
        }
        result.sort(function(left, right) {
            var leftDownloaded = isDownloaded(providerKey, left.version)
            var rightDownloaded = isDownloaded(providerKey, right.version)
            var leftDownloading = providerController.busy
                    && providerController.activeProvider === providerKey
                    && providerController.activeVersion === left.version
                    && !leftDownloaded
            var rightDownloading = providerController.busy
                    && providerController.activeProvider === providerKey
                    && providerController.activeVersion === right.version
                    && !rightDownloaded
            var leftPriority = leftDownloading ? 0 : leftDownloaded ? 1 : 2
            var rightPriority = rightDownloading ? 0 : rightDownloaded ? 1 : 2
            if (leftPriority !== rightPriority)
                return leftPriority - rightPriority
            return compareVersionsDescending(left, right)
        })
        return result
    }

    function compareVersionsDescending(left, right) {
        var leftText = String(left.version || "").replace(/^v/i, "")
        var rightText = String(right.version || "").replace(/^v/i, "")
        var leftParts = leftText.split("-", 2)
        var rightParts = rightText.split("-", 2)
        var leftNumbers = leftParts[0].split(".")
        var rightNumbers = rightParts[0].split(".")
        var count = Math.max(leftNumbers.length, rightNumbers.length)

        for (var i = 0; i < count; ++i) {
            var leftNumber = i < leftNumbers.length ? parseInt(leftNumbers[i], 10) : 0
            var rightNumber = i < rightNumbers.length ? parseInt(rightNumbers[i], 10) : 0
            if (isNaN(leftNumber) || isNaN(rightNumber))
                break
            if (leftNumber !== rightNumber)
                return rightNumber - leftNumber
        }

        var leftPrerelease = leftParts.length > 1
        var rightPrerelease = rightParts.length > 1
        if (leftPrerelease !== rightPrerelease)
            return leftPrerelease ? 1 : -1
        return rightText.localeCompare(leftText)
    }

    function selectedItem() {
        return sdkCatalog[selectedSdk]
    }

    function isDownloaded(sdkKey, version) {
        return downloadedVersions[sdkKey + ":" + version] === true
                || providerController.isDownloaded(sdkKey, version)
    }

    function installedCount() {
        var count = 0
        var item = selectedItem()
        for (var i = 0; i < displayedVersions.length; ++i) {
            if (isDownloaded(item.key, displayedVersions[i].version))
                ++count
        }
        return count
    }

    function commandHint() {
        if (currentSection === "download" && selectedItem().section === "sdk")
            return qsTr("安装后，在项目目录执行  svm use %1 <version>  即可切换版本。").arg(selectedItem().key)
        return qsTr("使用  svm install %1 <version>  也可以从命令行安装。").arg(selectedItem().key)
    }

    function download(sdkKey, version) {
        providerController.download(sdkKey, version)
    }

    Timer {
        id: toastTimer
        interval: 2600
        onTriggered: toastText = ""
    }

    Dialog {
        id: deleteDialog
        anchors.centerIn: parent
        width: 420
        modal: true
        closePolicy: Popup.NoAutoClose
        padding: 0

        background: Rectangle {
            radius: 12
            color: "#151b24"
            border.width: 1
            border.color: "#343e4d"
        }

        contentItem: ColumnLayout {
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 22
                spacing: 10

                Text {
                    text: qsTr("确认删除")
                    color: "#f0f4fb"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("确定删除 %1 %2？下载包和安装目录都会被删除。")
                          .arg(window.pendingDeleteName)
                          .arg(window.pendingDeleteVersion)
                    color: "#a5b0c1"
                    font.pixelSize: 13
                }
                Text {
                    Layout.fillWidth: true
                    visible: providerController.defaultVersions[window.pendingDeleteProvider]
                             === window.pendingDeleteVersion
                    wrapMode: Text.WordWrap
                    text: qsTr("这是当前默认版本，对应命令指向也会一并移除。")
                    color: "#e6b86f"
                    font.pixelSize: 12
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#2a3240"
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                spacing: 10
                Item { Layout.fillWidth: true }

                SecondaryButton {
                    text: qsTr("取消")
                    onClicked: deleteDialog.close()
                }

                Button {
                    id: confirmDeleteButton
                    implicitWidth: 92
                    implicitHeight: 38

                    contentItem: Text {
                        text: qsTr("确认删除")
                        color: "#fff1f1"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: confirmDeleteButton.pressed ? "#a93e49"
                              : confirmDeleteButton.hovered ? "#d0505d" : "#bd4652"
                    }
                    onClicked: {
                        deleteDialog.close()
                        providerController.removeDownloaded(window.pendingDeleteProvider,
                                                            window.pendingDeleteVersion)
                    }
                }
            }
        }
    }

    Connections {
        target: providerController

        function onDownloadFinished(providerId, version, path) {
            var next = Object.assign({}, downloadedVersions)
            next[providerId + ":" + version] = true
            downloadedVersions = next
            toastText = qsTr("%1 %2 下载并校验完成").arg(providerId).arg(version)
            toastTimer.restart()
        }

        function onDownloadRemoved(providerId, version) {
            var next = Object.assign({}, downloadedVersions)
            delete next[providerId + ":" + version]
            downloadedVersions = next
            toastText = qsTr("%1 %2 已删除").arg(providerId).arg(version)
            toastTimer.restart()
        }

        function onErrorChanged() {
            if (providerController.error !== "") {
                toastText = providerController.error
                toastTimer.restart()
            }
        }
    }

    component NavButton: Button {
        id: navButton
        property string iconText: ""
        property bool current: false

        implicitHeight: 44
        Layout.fillWidth: true
        flat: true

        contentItem: RowLayout {
            spacing: 12

            Text {
                text: navButton.iconText
                color: navButton.current ? "#8cc8ff" : "#778195"
                font.pixelSize: 16
                Layout.preferredWidth: 20
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                text: navButton.text
                color: navButton.current ? "#eef5ff" : "#9aa4b7"
                font.pixelSize: 14
                font.weight: navButton.current ? Font.DemiBold : Font.Normal
                Layout.fillWidth: true
            }
        }

        background: Rectangle {
            radius: 9
            color: navButton.current ? "#17283b"
                                     : navButton.hovered ? "#141923" : "transparent"
            border.width: navButton.current ? 1 : 0
            border.color: "#25405e"
        }
    }

    component SecondaryButton: Button {
        id: secondaryButton
        implicitHeight: 38
        leftPadding: 15
        rightPadding: 15

        contentItem: Text {
            text: secondaryButton.text
            color: "#c9d3e3"
            font.pixelSize: 13
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: 8
            color: secondaryButton.pressed ? "#242c3a"
                                           : secondaryButton.hovered ? "#1d2430" : "#171c25"
            border.width: 1
            border.color: "#2a3240"
        }
    }

    component FilterButton: Button {
        id: filterButton
        property bool current: false

        implicitWidth: 68
        implicitHeight: 34
        leftPadding: 13
        rightPadding: 13

        contentItem: Text {
            text: filterButton.text
            color: filterButton.current ? "#9ed4ff" : "#7e899b"
            font.pixelSize: 11
            font.weight: filterButton.current ? Font.DemiBold : Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: 7
            color: filterButton.current ? "#17283b"
                  : filterButton.hovered ? "#171e28" : "transparent"
            border.width: 1
            border.color: filterButton.current ? "#2c5680" : "#29313d"
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 218
            Layout.fillHeight: true
            color: "#0d1118"
            border.width: 0

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: "#202631"
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 23
                    Layout.topMargin: 3
                    spacing: 11

                    Rectangle {
                        width: 38
                        height: 38
                        radius: 11
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#50a9ff" }
                            GradientStop { position: 1.0; color: "#796bff" }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: "S"
                            color: "white"
                            font.pixelSize: 19
                            font.weight: Font.Bold
                        }
                    }

                    Column {
                        Layout.fillWidth: true
                        spacing: 1

                        Text {
                            text: "SVM"
                            color: "#f2f6fd"
                            font.pixelSize: 17
                            font.weight: Font.Bold
                        }

                        Text {
                            text: qsTr("SDK Version Manager")
                            color: "#697488"
                            font.pixelSize: 10
                        }
                    }
                }

                Text {
                    text: qsTr("功能")
                    color: "#556176"
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.4
                    Layout.leftMargin: 10
                    Layout.bottomMargin: 3
                }

                NavButton {
                    text: qsTr("下载")
                    iconText: "↓"
                    current: window.currentSection === "download"
                    onClicked: window.selectSection("download")
                }
                NavButton {
                    text: qsTr("Web")
                    iconText: "◎"
                    current: window.currentSection === "web"
                    onClicked: window.selectSection("web")
                }
                NavButton {
                    text: qsTr("数据库")
                    iconText: "▱"
                    current: window.currentSection === "database"
                    onClicked: window.selectSection("database")
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 78
                    radius: 10
                    color: "#111722"
                    border.width: 1
                    border.color: "#232b38"

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 5

                        RowLayout {
                            Text {
                                text: "●"
                                color: "#5bd894"
                                font.pixelSize: 10
                            }
                            Text {
                                text: qsTr("核心服务正常")
                                color: "#c9d4e4"
                                font.pixelSize: 12
                            }
                        }

                        Text {
                            text: qsTr("已安装 2 个 SDK")
                            color: "#657186"
                            font.pixelSize: 11
                        }
                    }
                }

                NavButton { text: qsTr("设置"); iconText: "⚙" }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 76
                color: "#0b0e14"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 30
                    anchors.rightMargin: 30
                    spacing: 14

                    Column {
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            text: window.sectionInfo.title
                            color: "#f0f4fb"
                            font.pixelSize: 22
                            font.weight: Font.DemiBold
                        }

                        Text {
                            text: window.sectionInfo.description
                            color: "#737e91"
                            font.pixelSize: 12
                        }
                    }

                    SecondaryButton {
                        text: "↻  " + qsTr("刷新版本")
                        onClicked: {
                            providerController.loadVersions(window.selectedItem().key, true)
                        }
                    }

                    SecondaryButton {
                        text: "⌘  " + qsTr("打开终端")
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#1d232e"
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.preferredWidth: 282
                    Layout.fillHeight: true
                    color: "#0d1118"

                    Rectangle {
                        anchors.right: parent.right
                        width: 1
                        height: parent.height
                        color: "#202631"
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 12

                        Text {
                            text: window.sectionInfo.listTitle
                            color: "#8290a5"
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.2
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 40
                            radius: 8
                            color: "#121720"
                            border.width: 1
                            border.color: searchField.activeFocus ? "#3c7bb5" : "#272e3a"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 8

                                Text {
                                    text: "⌕"
                                    color: "#6f7b8e"
                                    font.pixelSize: 17
                                }

                                TextField {
                                    id: searchField
                                    Layout.fillWidth: true
                                    placeholderText: window.sectionInfo.searchHint
                                    placeholderTextColor: "#596477"
                                    color: "#dce5f2"
                                    font.pixelSize: 13
                                    background: null
                                    onTextChanged: window.searchText = text
                                }
                            }
                        }

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                            Column {
                                width: parent.width
                                spacing: 7

                                Rectangle {
                                    width: parent.width
                                    height: 86
                                    visible: window.filteredCatalog.length === 0
                                    radius: 9
                                    color: "#111720"
                                    border.width: 1
                                    border.color: "#252d39"

                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 6

                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: qsTr("没有匹配的组件")
                                            color: "#a8b2c2"
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: qsTr("删除关键词可恢复完整列表")
                                            color: "#5f6b7e"
                                            font.pixelSize: 10
                                        }
                                    }
                                }

                                Repeater {
                                    model: window.filteredCatalog

                                    delegate: Item {
                                        id: sdkDelegate
                                        required property int index
                                        required property var modelData
                                        width: parent.width
                                        height: 66

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 10
                                            color: sdkMouse.containsMouse
                                                   || window.selectedItem().key === modelData.key
                                                   ? "#17202c" : "transparent"
                                            border.width: window.selectedItem().key === modelData.key ? 1 : 0
                                            border.color: "#293a4e"

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 10
                                                spacing: 11

                                                Rectangle {
                                                    width: 39
                                                    height: 39
                                                    radius: 10
                                                    color: Qt.alpha(modelData.color, 0.14)
                                                    border.width: 1
                                                    border.color: Qt.alpha(modelData.color, 0.36)

                                                    Text {
                                                        anchors.centerIn: parent
                                                        text: modelData.mark
                                                        color: modelData.color
                                                        font.pixelSize: modelData.mark.length > 1 ? 13 : 17
                                                        font.weight: Font.Bold
                                                    }
                                                }

                                                Column {
                                                    Layout.fillWidth: true
                                                    spacing: 4

                                                    Text {
                                                        text: modelData.name
                                                        color: "#e3eaf4"
                                                        font.pixelSize: 14
                                                        font.weight: Font.Medium
                                                    }

                                                    Text {
                                                        text: modelData.summary
                                                        color: "#657185"
                                                        font.pixelSize: 11
                                                    }
                                                }

                                                Text {
                                                    text: "›"
                                                    color: window.selectedItem().key === modelData.key
                                                           ? modelData.color : "#4e596b"
                                                    font.pixelSize: 20
                                                }
                                            }

                                            MouseArea {
                                                id: sdkMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: window.selectProvider(modelData.key)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#0b0e14"

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 28
                        anchors.rightMargin: 30
                        anchors.topMargin: 24
                        anchors.bottomMargin: 22
                        spacing: 18

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 14

                            Rectangle {
                                width: 50
                                height: 50
                                radius: 13
                                color: Qt.alpha(window.selectedItem().color, 0.14)
                                border.width: 1
                                border.color: Qt.alpha(window.selectedItem().color, 0.34)

                                Text {
                                    anchors.centerIn: parent
                                    text: window.selectedItem().mark
                                    color: window.selectedItem().color
                                    font.pixelSize: window.selectedItem().mark.length > 1 ? 16 : 22
                                    font.weight: Font.Bold
                                }
                            }

                            Column {
                                Layout.fillWidth: true
                                spacing: 5

                                Text {
                                    text: window.selectedItem().name
                                    color: "#f0f4fb"
                                    font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                }

                                Text {
                                    text: qsTr("选择一个版本进行下载和安装")
                                    color: "#717d90"
                                    font.pixelSize: 12
                                }
                            }

                            RowLayout {
                                Layout.preferredWidth: 500
                                visible: window.currentSection === "download"
                                spacing: 8

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 36
                                    radius: 8
                                    color: "#10151d"
                                    border.width: 1
                                    border.color: versionSearchField.activeFocus ? "#3c7bb5" : "#29313d"

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 11
                                        anchors.rightMargin: 8
                                        spacing: 7

                                        Text {
                                            text: "⌕"
                                            color: "#697589"
                                            font.pixelSize: 16
                                        }

                                        TextField {
                                            id: versionSearchField
                                            Layout.fillWidth: true
                                            placeholderText: qsTr("搜索版本，例如 22.17")
                                            placeholderTextColor: "#596477"
                                            color: "#dce5f2"
                                            font.pixelSize: 12
                                            background: null
                                            onTextChanged: window.versionSearchText = text
                                        }

                                        Text {
                                            visible: versionSearchField.text.length > 0
                                            text: "×"
                                            color: clearVersionMouse.containsMouse ? "#b8c5d8" : "#6c788a"
                                            font.pixelSize: 16

                                            MouseArea {
                                                id: clearVersionMouse
                                                anchors.fill: parent
                                                anchors.margins: -7
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: versionSearchField.clear()
                                            }
                                        }
                                    }
                                }

                                FilterButton {
                                    text: qsTr("全部")
                                    current: window.versionFilter === "all"
                                    onClicked: window.versionFilter = "all"
                                }
                                FilterButton {
                                    text: "LTS"
                                    visible: window.selectedItem().key === "node"
                                             || window.selectedItem().key === "java"
                                    current: window.versionFilter === "lts"
                                    onClicked: window.versionFilter = "lts"
                                }
                                FilterButton {
                                    text: "Current"
                                    visible: window.selectedItem().key === "node"
                                    current: window.versionFilter === "current"
                                    onClicked: window.versionFilter = "current"
                                }
                                FilterButton {
                                    text: "Stable"
                                    visible: window.selectedItem().key === "flutter"
                                    current: window.versionFilter === "stable"
                                    onClicked: window.versionFilter = "stable"
                                }
                                FilterButton {
                                    text: "Beta"
                                    visible: window.selectedItem().key === "flutter"
                                    current: window.versionFilter === "beta"
                                    onClicked: window.versionFilter = "beta"
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 41
                            visible: window.currentSection === "download"
                            radius: 8
                            color: "#10151d"
                            border.width: 1
                            border.color: "#202733"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 16
                                anchors.rightMargin: 14

                                Text {
                                    Layout.preferredWidth: 150
                                    text: qsTr("版本")
                                    color: "#697589"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.preferredWidth: 100
                                    text: qsTr("通道")
                                    color: "#697589"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("发布日期")
                                    color: "#697589"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.preferredWidth: 65
                                    text: qsTr("大小")
                                    color: "#697589"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                                Item { Layout.preferredWidth: 214 }
                            }
                        }

                        ScrollView {
                            id: versionScroll
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: window.currentSection === "download"
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                            Column {
                                width: versionScroll.availableWidth
                                spacing: 9

                                Rectangle {
                                    width: parent.width
                                    height: 150
                                    radius: 10
                                    visible: window.filteredVersions.length === 0
                                    color: "#0f141c"
                                    border.width: 1
                                    border.color: "#202733"

                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 8

                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: providerController.busy
                                                  && providerController.activeProvider === window.selectedItem().key
                                                  ? "↻" : "◇"
                                            color: window.selectedItem().color
                                            font.pixelSize: 25
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: providerController.busy
                                                  && providerController.activeProvider === window.selectedItem().key
                                                  ? qsTr("正在刷新 %1 版本").arg(window.selectedItem().name)
                                                  : window.displayedVersions.length === 0
                                                  ? window.hasVersionProvider(window.selectedItem().key)
                                                    ? qsTr("暂无可显示的 %1 版本").arg(window.selectedItem().name)
                                                    : qsTr("%1 Provider 即将支持").arg(window.selectedItem().name)
                                                  : qsTr("没有匹配的版本")
                                            color: "#aab5c6"
                                            font.pixelSize: 13
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: providerController.busy
                                                  && providerController.activeProvider === window.selectedItem().key
                                                  ? qsTr("正在读取官方版本索引，请稍候")
                                                  : window.displayedVersions.length === 0
                                                  ? window.hasVersionProvider(window.selectedItem().key)
                                                    ? qsTr("请点击右上角“刷新版本”重新获取")
                                                    : qsTr("目录已预留，接入 Provider 后会自动显示版本")
                                                  : qsTr("请更改搜索词或通道筛选")
                                            color: "#626f82"
                                            font.pixelSize: 11
                                        }
                                    }
                                }

                                Repeater {
                                    model: window.filteredVersions

                                    delegate: Rectangle {
                                        id: versionRow
                                        required property int index
                                        required property var modelData
                                        property bool installed: window.isDownloaded(window.selectedItem().key, modelData.version)
                                        property bool defaultVersion: providerController.defaultVersions[
                                                                          window.selectedItem().key]
                                                                      === modelData.version

                                        width: parent.width
                                        height: 69
                                        radius: 10
                                        color: versionMouse.containsMouse ? "#121923" : "#0f141c"
                                        border.width: 1
                                        border.color: installed ? "#244b3b"
                                                                : versionMouse.containsMouse ? "#2a3748" : "#202733"

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 16
                                            anchors.rightMargin: 13
                                            spacing: 0

                                            RowLayout {
                                                Layout.preferredWidth: 150
                                                spacing: 7

                                                Text {
                                                    text: modelData.version
                                                    color: "#e4ebf5"
                                                    font.pixelSize: 14
                                                    font.weight: Font.Medium
                                                }

                                                Rectangle {
                                                    visible: modelData.recommended
                                                    implicitWidth: recommendedText.implicitWidth + 12
                                                    implicitHeight: 21
                                                    radius: 5
                                                    color: "#172c42"

                                                    Text {
                                                        id: recommendedText
                                                        anchors.centerIn: parent
                                                        text: qsTr("推荐")
                                                        color: "#79bfff"
                                                        font.pixelSize: 9
                                                        font.weight: Font.DemiBold
                                                    }
                                                }
                                            }

                                            Item {
                                                Layout.preferredWidth: 100
                                                implicitHeight: 24

                                                Rectangle {
                                                    width: channelText.implicitWidth + 16
                                                    height: 24
                                                    radius: 6
                                                    color: modelData.channel === "beta" ? "#2b2037" : "#19221f"

                                                    Text {
                                                        id: channelText
                                                        anchors.centerIn: parent
                                                        text: modelData.channel
                                                        color: modelData.channel === "beta" ? "#c59bea" : "#8db49f"
                                                        font.pixelSize: 10
                                                    }
                                                }
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData.released
                                                color: "#778296"
                                                font.pixelSize: 12
                                            }

                                            Text {
                                                Layout.preferredWidth: 65
                                                text: modelData.size
                                                color: "#778296"
                                                font.pixelSize: 12
                                            }

                                            RowLayout {
                                                Layout.preferredWidth: 214
                                                spacing: 8

                                                Item {
                                                    Layout.fillWidth: true
                                                    visible: !versionRow.installed
                                                }

                                                Button {
                                                    id: defaultButton
                                                    visible: versionRow.installed
                                                    Layout.preferredWidth: 102
                                                    implicitHeight: 36
                                                    enabled: !versionRow.defaultVersion && !providerController.busy

                                                    contentItem: Text {
                                                        text: versionRow.defaultVersion
                                                              ? qsTr("★  默认版本")
                                                              : qsTr("设为默认")
                                                        color: versionRow.defaultVersion ? "#f2c96d" : "#a9bad0"
                                                        font.pixelSize: 11
                                                        font.weight: Font.DemiBold
                                                        horizontalAlignment: Text.AlignHCenter
                                                        verticalAlignment: Text.AlignVCenter
                                                    }

                                                    background: Rectangle {
                                                        radius: 8
                                                        color: versionRow.defaultVersion ? "#2a2416"
                                                              : defaultButton.hovered ? "#202938" : "#171e28"
                                                        border.width: 1
                                                        border.color: versionRow.defaultVersion ? "#655224" : "#344154"
                                                    }

                                                    onClicked: providerController.setDefaultVersion(
                                                                   window.selectedItem().key,
                                                                   modelData.version)
                                                }

                                                Button {
                                                    id: downloadButton
                                                    Layout.preferredWidth: 102
                                                    implicitHeight: 36
                                                    enabled: !providerController.busy

                                                    contentItem: Text {
                                                        text: providerController.busy
                                                                && providerController.activeProvider
                                                                   === window.selectedItem().key
                                                                && providerController.activeVersion === modelData.version
                                                              ? versionRow.installed
                                                                ? qsTr("删除中…")
                                                                : qsTr("%1%").arg(Math.round(providerController.progress * 100))
                                                              : versionRow.installed ? qsTr("删除")
                                                              : qsTr("↓  下载")
                                                        color: versionRow.installed ? "#ef8b8b" : "#08111b"
                                                        font.pixelSize: 12
                                                        font.weight: Font.DemiBold
                                                        horizontalAlignment: Text.AlignHCenter
                                                        verticalAlignment: Text.AlignVCenter
                                                    }

                                                    background: Rectangle {
                                                        radius: 8
                                                        color: versionRow.installed
                                                              ? downloadButton.hovered ? "#3a1d22" : "#26171b"
                                                              : downloadButton.pressed ? "#6ab1ee"
                                                              : downloadButton.hovered ? "#9ed4ff" : "#83c7ff"
                                                    border.width: versionRow.installed ? 1 : 0
                                                        border.color: "#633039"
                                                    }

                                                    onClicked: {
                                                        if (versionRow.installed) {
                                                            window.pendingDeleteProvider =
                                                                window.selectedItem().key
                                                            window.pendingDeleteVersion = modelData.version
                                                            window.pendingDeleteName =
                                                                window.selectedItem().name
                                                            deleteDialog.open()
                                                        } else {
                                                            window.download(window.selectedItem().key,
                                                                            modelData.version)
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        MouseArea {
                                            id: versionMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            acceptedButtons: Qt.NoButton
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: window.currentSection !== "download"
                            radius: 12
                            color: "#0f141c"
                            border.width: 1
                            border.color: "#202733"

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 24
                                spacing: 18

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14

                                    Rectangle {
                                        width: 42
                                        height: 42
                                        radius: 11
                                        color: window.installedCount() > 0 ? "#14271f" : "#24201a"
                                        border.width: 1
                                        border.color: window.installedCount() > 0 ? "#27523e" : "#51432d"

                                        Text {
                                            anchors.centerIn: parent
                                            text: window.installedCount() > 0 ? "✓" : "!"
                                            color: window.installedCount() > 0 ? "#6cd39e" : "#e0b76d"
                                            font.pixelSize: 17
                                            font.weight: Font.Bold
                                        }
                                    }

                                    Column {
                                        Layout.fillWidth: true
                                        spacing: 4

                                        Text {
                                            text: window.installedCount() > 0
                                                  ? qsTr("%1 已安装").arg(window.selectedItem().name)
                                                  : qsTr("%1 尚未安装").arg(window.selectedItem().name)
                                            color: "#e4ebf5"
                                            font.pixelSize: 16
                                            font.weight: Font.DemiBold
                                        }

                                        Text {
                                            text: window.installedCount() > 0
                                                  ? qsTr("可以创建配置并管理服务")
                                                  : qsTr("请先前往“下载”菜单选择版本并安装")
                                            color: "#748095"
                                            font.pixelSize: 12
                                        }
                                    }

                                    SecondaryButton {
                                        text: qsTr("前往下载")
                                        visible: window.installedCount() === 0
                                        onClicked: window.selectSection("download")
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 1
                                    color: "#202733"
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 2
                                    columnSpacing: 12
                                    rowSpacing: 12
                                    enabled: window.installedCount() > 0
                                    opacity: enabled ? 1.0 : 0.42

                                    Repeater {
                                        model: window.currentSection === "web"
                                               ? [qsTr("站点配置"), qsTr("端口与域名"), qsTr("服务状态"), qsTr("日志")]
                                               : [qsTr("实例管理"), qsTr("端口与目录"), qsTr("服务状态"), qsTr("备份与日志")]

                                        delegate: Rectangle {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            implicitHeight: 72
                                            radius: 9
                                            color: "#121923"
                                            border.width: 1
                                            border.color: "#25303e"

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: 16
                                                anchors.rightMargin: 14

                                                Text {
                                                    text: modelData
                                                    color: "#c9d3e3"
                                                    font.pixelSize: 13
                                                    Layout.fillWidth: true
                                                }
                                                Text {
                                                    text: "›"
                                                    color: "#718096"
                                                    font.pixelSize: 19
                                                }
                                            }
                                        }
                                    }
                                }

                                Item { Layout.fillHeight: true }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 54
                            visible: window.currentSection === "download"
                            radius: 9
                            color: "#0f1721"
                            border.width: 1
                            border.color: "#1d2b3c"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 15
                                anchors.rightMargin: 15
                                spacing: 10

                                Text {
                                    text: "ⓘ"
                                    color: "#6caee8"
                                    font.pixelSize: 15
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: providerController.busy
                                          && providerController.activeProvider === window.selectedItem().key
                                          ? providerController.status
                                          : window.commandHint()
                                    color: "#8290a4"
                                    font.pixelSize: 11
                                }

                                Text {
                                    text: providerController.busy
                                          ? qsTr("取消")
                                          : qsTr("查看命令 →")
                                    color: "#78bfff"
                                    font.pixelSize: 11

                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -8
                                        enabled: providerController.busy
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: providerController.cancel()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: toastLabel.implicitWidth + 34
        height: 42
        radius: 10
        visible: window.toastText !== ""
        color: "#202938"
        border.width: 1
        border.color: "#35455b"

        Text {
            id: toastLabel
            anchors.centerIn: parent
            text: window.toastText
            color: "#e5edf8"
            font.pixelSize: 12
        }
    }
}
