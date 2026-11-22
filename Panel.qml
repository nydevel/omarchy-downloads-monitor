import QtQuick
import QtQuick.Shapes
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// Bar widget: an animated arrow while anything downloads in the background,
// and a popup listing each download with its progress.
Panel {
  id: root
  moduleName: "io.github.nydevel.downloads-monitor"
  ipcTarget: "io.github.nydevel.downloads-monitor"

  readonly property string pluginDir: decodeURIComponent(String(Qt.resolvedUrl(".")).replace(/^file:\/\//, "")).replace(/\/$/, "")
  // Builds downloads.cpp on first use, then runs it.
  readonly property string helperPath: pluginDir + "/run.sh"
  property int quickExits: 0
  property real startedAt: 0

  readonly property bool alwaysShow: setting("alwaysShow", false) === true
  readonly property bool showSpeed: setting("showSpeed", false) === true
  readonly property bool notifyOnFinish: setting("notifyOnFinish", true) !== false
  readonly property string extraProcesses: Model.cleanProcessList(String(setting("extraProcesses", "")))

  property var jobs: []
  property real netRate: 0
  readonly property int jobCount: jobs.length
  readonly property bool active: jobCount > 0
  readonly property bool shown: active || alwaysShow || opened
  readonly property real totalRate: Model.totalRate(jobs)

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  function rateText(rate, approx) {
    return Model.formatRate(rate, Commons.I18n.formatNumber, approx)
  }

  function bytesText(bytes) {
    return Model.formatBytes(bytes, Commons.I18n.formatNumber)
  }

  function applySnapshot(line) {
    var snap = Model.parseSnapshot(line)
    if (!snap) return
    root.jobs = snap.jobs
    root.netRate = snap.rx
    if (root.notifyOnFinish && root.bar) {
      for (var i = 0; i < snap.finished.length; i++) {
        root.bar.run("omarchy-notification-send -g 󰇚 "
          + Util.shellQuote(strings.t("notify.finished"))
          + " " + Util.shellQuote(Model.notificationSafe(snap.finished[i])))
      }
    }
  }

  function restartMonitor() {
    monitor.running = false
    Qt.callLater(function() { monitor.running = true })
  }

  onExtraProcessesChanged: restartMonitor()

  visible: shown
  implicitWidth: shown ? button.implicitWidth : 0
  implicitHeight: shown ? button.implicitHeight : 0

  Process {
    id: monitor
    command: ["sh", root.helperPath, "--watch", root.extraProcesses]
    running: true
    // Process already has a C++ slot named onStarted, so track starts here.
    onRunningChanged: if (running) root.startedAt = Date.now()
    stdout: SplitParser {
      onRead: function(line) {
        root.quickExits = 0
        root.applySnapshot(line)
      }
    }
    onExited: {
      root.jobs = []
      // A helper that dies right away (e.g. no compiler) backs off to a
      // minute instead of rebuilding every few seconds.
      if (Date.now() - root.startedAt < 3000) root.quickExits++
      respawn.interval = root.quickExits >= 3 ? 60000 : 5000
      respawn.restart()
    }
  }

  Timer {
    id: respawn
    interval: 5000
    onTriggered: if (!monitor.running) monitor.running = true
  }

  TextMetrics {
    id: speedMetrics
    font.family: root.bar ? root.bar.fontFamily : Style.fontFamily
    font.pixelSize: Style.bar.iconFont
    text: "000.0 MB/s"
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    iconComponent: arrowIcon
    labelText: root.showSpeed && root.active && !vertical ? root.rateText(root.totalRate, false) : ""
    slotSize: Style.bar.iconSlot + (labelText !== "" ? speedMetrics.advanceWidth + Style.space(4) : 0)
    tooltipText: root.opened ? "" : (root.active
      ? strings.tn("bar.tooltip", root.jobCount).arg(Commons.I18n.formatNumber(root.jobCount)).arg(root.rateText(root.totalRate, false))
      : strings.t("bar.idle"))
    onPressed: function(b) { root.toggle() }
  }

  // Arrow dropping into a tray. Drawn from shapes so it follows the theme
  // colour and needs no particular icon font.
  Component {
    id: arrowIcon

    Rectangle {
      id: icon
      readonly property real s: Style.bar.iconSize
      readonly property real stroke: Math.max(1.5, Math.round(s * 0.15))
      readonly property color ink: root.bar ? root.bar.foreground : Commons.Color.foreground
      readonly property bool moving: root.active && !Style.reduceMotion
      width: s
      height: s
      color: "transparent"

      Item {
        id: lane
        width: parent.width
        height: parent.height - icon.stroke - Math.max(1, icon.s * 0.08)
        clip: true

        Item {
          id: arrow
          width: lane.width
          height: lane.height * 0.92
          y: icon.moving ? 0 : lane.height - height
          opacity: root.active ? 1 : 0.55

          Rectangle {
            x: (parent.width - width) / 2
            width: icon.stroke
            height: parent.height * 0.62
            radius: width / 2
            color: icon.ink
          }

          Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
              strokeWidth: 0
              strokeColor: "transparent"
              fillColor: icon.ink
              startX: arrow.width * 0.14; startY: arrow.height * 0.46
              PathLine { x: arrow.width * 0.86; y: arrow.height * 0.46 }
              PathLine { x: arrow.width * 0.5; y: arrow.height }
              PathLine { x: arrow.width * 0.14; y: arrow.height * 0.46 }
            }
          }

          // Constant motion, so linear: the arrow falls through the lane
          // and re-enters from the top.
          SequentialAnimation on y {
            running: icon.moving
            loops: Animation.Infinite
            NumberAnimation { from: -arrow.height; to: lane.height; duration: Style.duration(1100); easing.type: Easing.Linear }
          }
        }
      }

      Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: icon.stroke
        radius: height / 2
        color: icon.ink
      }
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(380))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight)

    content: PanelKeyCatcher {
      anchors.fill: parent
      implicitHeight: column.implicitHeight
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: Style.space(14)

        // ---------- Hero ----------
        Item {
          width: parent.width
          implicitHeight: Math.max(heroLabels.implicitHeight, heroRate.implicitHeight)

          Column {
            id: heroLabels
            anchors.left: parent.left
            anchors.right: heroRate.left
            anchors.rightMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            PlainText {
              width: parent.width
              text: strings.t("panel.title")
              font.pixelSize: Style.font.title
              font.bold: true
            }

            PlainText {
              width: parent.width
              text: (root.active
                ? strings.tn("panel.activeCount", root.jobCount).arg(Commons.I18n.formatNumber(root.jobCount))
                : strings.t("panel.idle")).toUpperCase()
              opacity: 0.6
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
            }
          }

          PlainText {
            id: heroRate
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: root.active ? root.rateText(root.totalRate, false) : ""
            font.pixelSize: Style.font.heading
            font.bold: true
          }
        }

        // ---------- Downloads ----------
        PlainText {
          visible: !root.active
          width: parent.width
          text: strings.t("panel.empty")
          opacity: 0.6
          wrapMode: Text.WordWrap
          font.pixelSize: Style.font.bodySmall
        }

        Repeater {
          model: root.jobs

          Column {
            id: jobItem
            required property var modelData
            readonly property var job: modelData
            readonly property real fraction: Model.fraction(job)
            width: column.width
            spacing: Style.space(6)

            PanelSeparator { foreground: root.bar.foreground }

            Item {
              width: parent.width
              implicitHeight: Math.max(jobTitle.implicitHeight, jobRate.implicitHeight)

              PlainText {
                id: jobTitle
                anchors.left: parent.left
                anchors.right: jobRate.left
                anchors.rightMargin: Style.space(10)
                text: jobItem.job.title
                font.pixelSize: Style.font.body
                font.bold: true
              }

              PlainText {
                id: jobRate
                anchors.right: parent.right
                text: jobItem.job.state === "writing" ? "" : root.rateText(jobItem.job.rate, jobItem.job.approx)
                font.pixelSize: Style.font.bodySmall
              }
            }

            PlainText {
              visible: text !== ""
              width: parent.width
              text: jobItem.job.detail
              opacity: 0.7
              font.pixelSize: Style.font.bodySmall
            }

            ProgressTrack {
              width: parent.width
              fraction: jobItem.fraction
              running: root.opened
            }

            PlainText {
              width: parent.width
              opacity: 0.6
              font.pixelSize: Style.font.caption
              text: Model.statusLine(jobItem.job, {
                bytes: root.bytesText,
                number: Commons.I18n.formatNumber,
                t: function(key) { return strings.t(key) },
                tn: function(key, n) { return strings.tn(key, n) }
              })
            }
          }
        }

        PlainText {
          visible: root.active && root.netRate > 0
          width: parent.width
          text: strings.t("panel.network").arg(root.rateText(root.netRate, false))
          opacity: 0.45
          font.pixelSize: Style.font.caption
        }
      }
    }
  }

  // Determinate bar when the size is known; a sliding segment otherwise.
  component ProgressTrack: Item {
    id: track
    property real fraction: -1
    property bool running: false
    readonly property bool known: fraction >= 0
    readonly property color ink: root.bar ? root.bar.foreground : Commons.Color.foreground
    implicitHeight: Style.space(6)
    clip: true

    Rectangle {
      anchors.fill: parent
      radius: height / 2
      color: Qt.rgba(track.ink.r, track.ink.g, track.ink.b, 0.12)
    }

    Rectangle {
      visible: track.known
      height: parent.height
      radius: height / 2
      color: track.ink
      width: Math.max(height, parent.width * Math.min(1, Math.max(0, track.fraction)))
      Behavior on width { NumberAnimation { duration: Style.duration(320); easing.type: Easing.OutCubic } }
    }

    Rectangle {
      id: slider
      visible: !track.known
      height: parent.height
      radius: height / 2
      width: parent.width * 0.28
      color: track.ink
      opacity: 0.8
      x: Style.reduceMotion ? (parent.width - width) / 2 : -width

      NumberAnimation on x {
        running: !track.known && track.running && !Style.reduceMotion
        loops: Animation.Infinite
        from: -slider.width
        to: track.width
        duration: Style.duration(1400)
        easing.type: Easing.Linear
      }
    }
  }

  component PlainText: Text {
    textFormat: Text.PlainText
    color: root.bar ? root.bar.foreground : Commons.Color.foreground
    font.family: root.bar ? root.bar.fontFamily : Style.fontFamily
    elide: Text.ElideRight
  }
}
