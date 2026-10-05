// make_snake_boxart.swift - draws the 446x614 box art shown in the cabinet's
// game menu.
//
// Usage: swift make_snake_boxart.swift <output.png> <snake-mascot.png>
//
// Run by tools/build.py; the result is shipped as boxart/boxart.png (named in
// cartridge.xml) and title.png links to it. AppKit draws with the origin at the
// bottom-left, so larger y values are nearer the top of the picture.
import AppKit
import Foundation

let output = URL(fileURLWithPath: CommandLine.arguments[1])
let mascotURL = URL(fileURLWithPath: CommandLine.arguments[2])
let width = 446
let height = 614
// An off-screen 8-bit RGBA canvas to draw into.
let bitmap = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: width,
    pixelsHigh: height,
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 0
)!

// Shorthand for an NSColor from 0-1 components.
func color(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(calibratedRed: red, green: green, blue: blue, alpha: alpha)
}

// Draws text with its bottom-left corner at (x, y) in a heavy monospaced font.
func label(_ message: String, x: CGFloat, y: CGFloat, size: CGFloat, fill: NSColor) {
    let text = message as NSString
    let attributes: [NSAttributedString.Key: Any] = [
        .font: NSFont.monospacedSystemFont(ofSize: size, weight: .black),
        .foregroundColor: fill
    ]
    text.draw(at: NSPoint(x: x, y: y), withAttributes: attributes)
}

// Draws text horizontally centred on the picture, in Arial Rounded Bold.
func title(_ message: String, y: CGFloat, size: CGFloat, fill: NSColor) {
    let font = NSFont(name: "ArialRoundedMTBold", size: size) ?? NSFont.boldSystemFont(ofSize: size)
    let attributes: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: fill
    ]
    let text = message as NSString
    let left = (CGFloat(width) - text.size(withAttributes: attributes).width) / 2
    text.draw(at: NSPoint(x: left, y: y), withAttributes: attributes)
}

NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
// Dark blue gradient, lighter towards the top.
let background = NSGradient(colors: [color(0.015, 0.035, 0.10), color(0.05, 0.12, 0.24)])!
background.draw(in: NSRect(x: 0, y: 0, width: width, height: height), angle: 90)

// Cyan bars along the bottom and top edges.
color(0.0, 0.87, 0.95).setFill()
NSRect(x: 0, y: 0, width: width, height: 12).fill()
NSRect(x: 0, y: 602, width: width, height: 12).fill()

// Title block near the top.
title("ALP", y: 555, size: 36, fill: color(0.98, 0.87, 0.16))
title("SNAKE", y: 480, size: 78, fill: color(0.05, 0.94, 0.80))

// A dark framed box for the mascot picture.
color(0.02, 0.07, 0.15).setFill()
NSRect(x: 25, y: 112, width: 396, height: 364).fill()
color(0.0, 0.84, 0.93).setStroke()
let border = NSBezierPath(rect: NSRect(x: 25, y: 112, width: 396, height: 364))
border.lineWidth = 6
border.stroke()

guard let mascot = NSImage(contentsOf: mascotURL) else {
    fatalError("Could not open snake mascot at \(mascotURL.path)")
}
mascot.draw(in: NSRect(x: 27, y: 117, width: 392, height: 355),
            from: .zero, operation: .sourceOver, fraction: 1)

// Tag lines under the box.
label("DUAL SCREEN", x: 52, y: 73, size: 28, fill: color(0.98, 0.87, 0.16))
label("PINBALL EDITION", x: 75, y: 37, size: 18, fill: .white)

// Finish drawing and save as PNG.
NSGraphicsContext.restoreGraphicsState()
try bitmap.representation(using: .png, properties: [:])!.write(to: output)
