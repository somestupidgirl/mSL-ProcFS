// Renders the procfs icon set.
//
// The mark is a process tree: a root that branches into children, which is what
// /proc actually contains. It doubles as a directory hierarchy, so it sits in
// the same visual family as a namespace-shaped project without copying it.
//
//   swift mkicons.swift <outdir>
//
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."

// ---------------------------------------------------------------- geometry --
// Tree laid out in a normalised 0...1 box, scaled to whatever canvas we draw.
// Root at top, then two branches; the left one forks again so the shape reads
// as a hierarchy rather than a symmetric ornament.
struct Node { let x: CGFloat, y: CGFloat, r: CGFloat }

// Orthogonal layout: a root, a horizontal bus, and drops to three children -
// the shape a tree view has. Right angles stay crisp at menu-bar size, where
// diagonals blur into grey.
let root   = Node(x: 0.50,  y: 0.105, r: 0.105)
let childL = Node(x: 0.115, y: 0.895, r: 0.085)
let childM = Node(x: 0.50,  y: 0.895, r: 0.085)
let childR = Node(x: 0.885, y: 0.895, r: 0.085)

let allNodes = [root, childL, childM, childR]
let busY: CGFloat = 0.52

/// Draw the tree into `rect` in `color`.
///
/// The layout is deliberately asymmetric - only the left branch forks - so its
/// bounding box is not centred on 0.5. Measure the box (including node radii
/// and half the connector width) and fit it to `rect`, otherwise the mark sits
/// visibly off to one side.
func drawTree(in rect: CGRect, color: NSColor) {
    let stroke: CGFloat = 0.062
    let pad = stroke / 2
    let minX = allNodes.map { $0.x - $0.r }.min()! - pad
    let maxX = allNodes.map { $0.x + $0.r }.max()! + pad
    let minY = allNodes.map { $0.y - $0.r }.min()! - pad
    let maxY = allNodes.map { $0.y + $0.r }.max()! + pad
    let scale = min(rect.width / (maxX - minX), rect.height / (maxY - minY))
    let drawnW = (maxX - minX) * scale
    let drawnH = (maxY - minY) * scale
    let originX = rect.minX + (rect.width - drawnW) / 2
    let originY = rect.minY + (rect.height - drawnH) / 2

    func pt(_ n: Node) -> CGPoint {
        // y runs downward in the layout; AppKit's origin is bottom-left.
        CGPoint(x: originX + (n.x - minX) * scale,
                y: originY + drawnH - (n.y - minY) * scale)
    }
    color.setStroke()
    color.setFill()

    let line = NSBezierPath()
    line.lineWidth = stroke * scale
    line.lineCapStyle = .round
    line.lineJoinStyle = .round

    // Trunk from the root down to the bus.
    line.move(to: pt(root))
    line.line(to: pt(Node(x: root.x, y: busY, r: 0)))
    // The bus itself, spanning the outer children.
    line.move(to: pt(Node(x: childL.x, y: busY, r: 0)))
    line.line(to: pt(Node(x: childR.x, y: busY, r: 0)))
    // Drops from the bus to each child.
    for c in [childL, childM, childR] {
        line.move(to: pt(Node(x: c.x, y: busY, r: 0)))
        line.line(to: pt(c))
    }
    line.stroke()

    // Nodes last, so they cap the connectors cleanly.
    for n in allNodes {
        let r = n.r * scale
        let c = pt(n)
        NSBezierPath(ovalIn: CGRect(x: c.x - r, y: c.y - r,
                                    width: 2 * r, height: 2 * r)).fill()
    }
}

// ------------------------------------------------------------------ canvas --
func render(size: Int, _ body: (CGRect) -> Void) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    NSGraphicsContext.current?.cgContext.setShouldAntialias(true)
    body(CGRect(x: 0, y: 0, width: CGFloat(size), height: CGFloat(size)))
    NSGraphicsContext.restoreGraphicsState()
    return rep
}

func write(_ rep: NSBitmapImageRep, _ name: String) {
    let url = URL(fileURLWithPath: out).appendingPathComponent(name)
    try! rep.representation(using: .png, properties: [:])!.write(to: url)
    print("  \(name)  \(rep.pixelsWide)x\(rep.pixelsHigh)")
}

// ---------------------------------------------------------------- app icon --
// macOS app icons sit on a rounded square inset from the canvas edge, with the
// corner radius Apple has used since Big Sur (~22.37% of the square's side).
let appIcon = render(size: 1024) { r in
    let inset = r.width * 0.094
    let box = r.insetBy(dx: inset, dy: inset)
    let radius = box.width * 0.2237
    let tile = NSBezierPath(roundedRect: box, xRadius: radius, yRadius: radius)

    // Shadow first, so the tile reads as a physical object like system icons.
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor(calibratedWhite: 0, alpha: 0.28)
    shadow.shadowBlurRadius = box.width * 0.05
    shadow.shadowOffset = NSSize(width: 0, height: -box.width * 0.02)
    shadow.set()
    NSColor.black.setFill()
    tile.fill()
    NSGraphicsContext.restoreGraphicsState()

    // Indigo-to-cyan: recognisably macOS, but distinct from the plain
    // system-blue folder the old icon borrowed.
    NSGraphicsContext.saveGraphicsState()
    tile.addClip()
    let grad = NSGradient(starting: NSColor(srgbRed: 0.278, green: 0.243, blue: 0.729, alpha: 1),
                          ending:   NSColor(srgbRed: 0.153, green: 0.647, blue: 0.855, alpha: 1))!
    grad.draw(in: box, angle: -90)

    // Soft highlight across the top edge, as Apple's own tiles have.
    let gloss = NSGradient(starting: NSColor(calibratedWhite: 1, alpha: 0.20),
                           ending:   NSColor(calibratedWhite: 1, alpha: 0.0))!
    gloss.draw(in: CGRect(x: box.minX, y: box.midY, width: box.width, height: box.height / 2),
               angle: -90)
    NSGraphicsContext.restoreGraphicsState()

    // The mark, centred in the tile with generous margins.
    let g = box.insetBy(dx: box.width * 0.20, dy: box.height * 0.20)
    drawTree(in: g, color: .white)
}
write(appIcon, "appicon.png")

// ------------------------------------------------------------- menu bar art --
// Drawn edge to edge (no tile): the status bar supplies its own spacing. Two
// variants are kept because the app picks the file itself rather than relying
// on template tinting.
for (name, color) in [("icon_dark.png", NSColor.black), ("icon_light.png", NSColor.white)] {
    let rep = render(size: 92) { r in
        drawTree(in: r.insetBy(dx: r.width * 0.10, dy: r.height * 0.10), color: color)
    }
    write(rep, name)
}
