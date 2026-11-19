import QtQuick
import qs.Commons
import qs.Ui

BarWidget {
  id: root
  readonly property PluginBarApi pluginBar: bar as PluginBarApi
  readonly property var spotify: pluginBar?.shell?.serviceFor("khdobromir.spotify") ?? null
  readonly property var sample: spotify?.sample ?? { status: "loading" }
  readonly property string icon: "󰓇"
  readonly property string trackLabel: sample.status !== "track" ? "" : (sample.artist ? sample.artist + " · " : "") + sample.title
  property bool popupOpen: false

  moduleName: "khdobromir.spotify"
  implicitWidth: trigger.implicitWidth
  implicitHeight: trigger.implicitHeight

  function close() {
    popupOpen = false
  }

  FontMetrics {
    id: metrics
    font.family: trigger.fontFamily
    font.pixelSize: trigger.fontSize
  }

  WidgetButton {
    id: trigger
    anchors.fill: parent
    bar: root.bar
    text: root.vertical || !root.trackLabel ? root.icon
      : root.icon + "  " + metrics.elidedText(root.trackLabel, Qt.ElideRight, Style.space(200))
    tooltipText: "Spotify"
    onPressed: function(button) {
      if (button === Qt.LeftButton)
        root.popupOpen = !root.popupOpen
    }
  }

  SpotifyCard {
    anchorItem: root
    bar: root.bar
    owner: root
    open: root.popupOpen
    spotify: root.spotify
    sample: root.sample
    spotifyIcon: root.icon
  }
}
