import "." as Skin
import Mixxx 1.0 as Mixxx
import QtQuick 2.12
import QtQuick.Controls
import QtQuick.Window 2.12
import "Theme"

ApplicationWindow {
    id: root

    readonly property bool isMobile: Qt.platform.os === "android" || Qt.platform.os === "ios"
    readonly property int designWidth: 1792
    readonly property int designHeight: 1008
    // Zydek: on phones and tablets ZyDeck's own pages (a WebView, see MainActivity.java) cover the screen in both
    // orientations, so Mixxx's window is never shown or built there: nothing to draw under them (that saves the
    // GPU and battery, and Android's renderer sometimes crashed when this window's surface appeared under the
    // WebView during start-up). The audio engine, library and controllers don't need it.
    property bool showMainWindow: !isMobile
    // Built the first time it's needed and then kept: Mixxx's interface can't be destroyed and built again
    // (a second instance crashes in QQmlConnections), so in portrait it's only hidden.
    property bool mainWindowBuilt: !isMobile
    onShowMainWindowChanged: if (showMainWindow) mainWindowBuilt = true

    property bool landscapeSeen: true

    Timer {
        // Rotations don't always change the window's size properties, so the screen is checked twice a
        // second, and the interface follows once a new orientation has held for two checks.
        interval: 500
        repeat: true
        running: false   // Zydek: was root.isMobile, see showMainWindow
        triggeredOnStart: true
        onTriggered: {
            const landscape = Screen.width >= Screen.height;
            if (landscape === root.landscapeSeen && landscape !== root.showMainWindow) {
                root.showMainWindow = landscape;
                console.log(`Zydek: screen ${Screen.width}x${Screen.height}: main window ${landscape ? "on" : "off"}`);
            }
            root.landscapeSeen = landscape;
        }
    }

    color: Theme.backgroundColor
    height: isMobile ? Screen.height : designHeight
    menuBar: content.item ? content.item.menuBar : null
    minimumHeight: isMobile ? 0 : 300
    minimumWidth: isMobile ? 0 : 680
    visible: !isMobile
    width: isMobile ? Screen.width : designWidth

    function updateVisibility() {
        if (!Mixxx.Core.ready || isMobile) {   // Zydek: stays hidden on mobile
            return;
        }
        root.visibility = Mixxx.Config.configStartInFullscreenKey || isMobile
                ? Window.FullScreen
                : Window.Windowed;
    }

    Connections {
        target: Mixxx.Core
        function onReadyChanged() {
            root.updateVisibility();
        }
    }

    Component.onCompleted: root.updateVisibility()

    Loader {
        id: content

        // While hidden (portrait) it keeps a landscape shape, so its narrow-screen layouts never run.
        height: root.showMainWindow ? root.height : Math.min(root.width, root.height)
        visible: root.showMainWindow
        width: root.showMainWindow ? root.width : Math.max(root.width, root.height)

        // Zydek: in portrait a phone shows the library page (a WebView over this window, see MainActivity.java),
        // so Mixxx's own interface is only built in landscape. Its narrow layouts crash while being built
        // (QQmlConnections during incubation), and nobody would see them anyway.
        // The orientation is only acted on once the window size has settled (it can flip while the app
        // starts), and on mobile the interface is built synchronously: cancelling a half-built one crashes too.
        active: Mixxx.Core.ready && root.mainWindowBuilt
        asynchronous: !root.isMobile
        onStatusChanged: {
            if (status === Loader.Error) {
                console.error("Failed to load the Mixxx main window")
                Qt.quit()
            }
        }
        sourceComponent: Component {
            MainWindow {
                applicationWindow: root
            }
        }
    }
    Rectangle {
        id: splash
        visible: opacity > 0
        color: Theme.backgroundColor
        anchors.fill: parent

        property bool ready: false

        Component.onCompleted: {
            ready = true
        }

        states: [
            State {
                when: splash.ready && content.status != Loader.Ready

                PropertyChanges {
                    text.opacity: 1
                    logo.opacity: 1
                    logo.y: root.height / 2 - logo.height / 2
                }
            },
            State {
                when: content.status == Loader.Ready && content.active

                PropertyChanges {
                    splash.opacity: 0
                }
            }
        ]
        Image {
            id: logo
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/images/mixxx-icon-logo-symbolic.svg"
            // height: 64
            opacity: 0
            y: root.height / 2

            Behavior on opacity {
                NumberAnimation { duration: 1500; easing.type: Easing.InOutQuad }
            }

            Behavior on y {
                NumberAnimation { duration: 1500; easing.type: Easing.InOutQuad }
            }
        }
        Text {
            id: text
            opacity: 0
            y: logo.y + logo.height*2
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 20
            font.pixelSize: 12
            color: Theme.lightGray3
            text: "DJ your way"
            Behavior on opacity {
                SequentialAnimation {
                    PauseAnimation { duration: 1000 }
                    NumberAnimation { duration: 500; easing.type: Easing.InOutQuad }
                }
            }
        }

        Behavior on opacity {
            NumberAnimation { duration: 500; easing.type: Easing.InOutQuad }
        }
    }
}
