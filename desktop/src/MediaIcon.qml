import QtQuick

Canvas {
    id: icon

    required property string name
    required property color color

    implicitWidth: 20
    implicitHeight: 20
    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    function lines(ctx, points) {
        ctx.beginPath()
        ctx.moveTo(points[0], points[1])
        for (let i = 2; i < points.length; i += 2)
            ctx.lineTo(points[i], points[i + 1])
        ctx.stroke()
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(width / 24, height / 24)
        ctx.fillStyle = color
        ctx.strokeStyle = color
        ctx.lineWidth = 1.65
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        switch (name) {
        case "play":
        case "next":
        case "previous":
            if (name === "previous") {
                ctx.translate(24, 0)
                ctx.scale(-1, 1)
            }
            ctx.beginPath()
            ctx.moveTo(7, 4.5)
            ctx.lineTo(name === "play" ? 20 : 17, 12)
            ctx.lineTo(7, 19.5)
            ctx.closePath()
            ctx.fill()
            if (name !== "play")
                ctx.fillRect(18, 5, 2, 14)
            break
        case "pause":
            ctx.fillRect(7, 5, 3.5, 14)
            ctx.fillRect(13.5, 5, 3.5, 14)
            break
        case "expand":
            lines(ctx, [8, 4, 4, 4, 4, 8])
            lines(ctx, [16, 4, 20, 4, 20, 8])
            lines(ctx, [20, 16, 20, 20, 16, 20])
            lines(ctx, [4, 16, 4, 20, 8, 20])
            break
        case "collapse":
            lines(ctx, [4, 8, 8, 8, 8, 4])
            lines(ctx, [16, 4, 16, 8, 20, 8])
            lines(ctx, [16, 20, 16, 16, 20, 16])
            lines(ctx, [4, 16, 8, 16, 8, 20])
            break
        case "back":
        case "forward":
            if (name === "forward") {
                ctx.translate(24, 0)
                ctx.scale(-1, 1)
            }
            lines(ctx, [14, 6, 8, 12, 14, 18])
            break
        case "add":
            lines(ctx, [5, 12, 19, 12])
            lines(ctx, [12, 5, 12, 19])
            break
        case "saved":
            lines(ctx, [5, 12, 10, 17, 19, 7])
            break
        case "close":
            lines(ctx, [6, 6, 18, 18])
            lines(ctx, [18, 6, 6, 18])
            break
        case "compact":
        case "grid": {
            const n = name === "compact" ? 3 : 2
            const tile = n === 3 ? 4 : 6.5
            for (let row = 0; row < n; ++row)
                for (let col = 0; col < n; ++col)
                    ctx.fillRect(4 + col * (tile + 2), 4 + row * (tile + 2), tile, tile)
            break
        }
        case "album":
            ctx.beginPath()
            ctx.arc(12, 12, 8, 0, 2 * Math.PI)
            ctx.stroke()
            ctx.beginPath()
            ctx.arc(12, 12, 2, 0, 2 * Math.PI)
            ctx.fill()
            break
        case "playlist":
            lines(ctx, [4, 6, 19, 6])
            lines(ctx, [4, 12, 19, 12])
            lines(ctx, [4, 18, 13, 18])
            break
        default:
            lines(ctx, [10, 17, 10, 6, 19, 4, 19, 14])
            ctx.beginPath()
            ctx.arc(7.5, 17, 2.5, 0, 2 * Math.PI)
            ctx.arc(16.5, 14, 2.5, 0, 2 * Math.PI)
            ctx.fill()
        }
    }
}
