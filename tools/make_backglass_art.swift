// make_backglass_art.swift - draws the 1920x1080 backglass background picture.
//
// Usage: swift make_backglass_art.swift <snake-mascot.png> <output.png> [--sample-scores]
//
// Run by tools/build.py at build time on the Mac (never on the cabinet). The PNG
// it writes is converted to an opaque BMP and shipped as
// boxart/snake-backglass.bmp; the backglass helper (src/snake_backglass.c) draws
// it first and then paints the live score panel over the lower part of it,
// which this picture therefore keeps mostly empty.
//
// --sample-scores also paints an example score panel, matching the helper's
// layout, for previewing the finished look without a cabinet.
//
// Note: AppKit's coordinate origin is the BOTTOM-left (y grows upwards),
// whereas SDL's is the top-left. So y 50-345 here is y 735-1030 in the helper.
import AppKit
import CoreText
import Foundation

let source = URL(fileURLWithPath: CommandLine.arguments[1])
let output = URL(fileURLWithPath: CommandLine.arguments[2])
let width = 1920
let height = 1080
// Nunito (assets/Nunito-wght.ttf, found relative to this script) is registered
// for this process only; the preview labels below use it.
let fontURL = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .deletingLastPathComponent().appendingPathComponent("assets/Nunito-wght.ttf")
_ = CTFontManagerRegisterFontsForURL(fontURL as CFURL, .process, nil)
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

// Draws text with its bottom-left corner at (x, y) in Nunito ExtraBold.
func label(_ message: String, x: CGFloat, y: CGFloat, size: CGFloat, fill: NSColor) {
    let attributes: [NSAttributedString.Key: Any] = [
        .font: NSFont(name: "Nunito-ExtraBold", size: size)
            ?? NSFont.boldSystemFont(ofSize: size),
        .foregroundColor: fill
    ]
    (message as NSString).draw(at: NSPoint(x: x, y: y), withAttributes: attributes)
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

// Make the bitmap the current drawing target.
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)

// Dark navy background, lighter towards the top (angle 90 = bottom to top).
NSGradient(colors: [color(0.025, 0.055, 0.13), color(0.085, 0.18, 0.31)])!
    .draw(in: NSRect(x: 0, y: 0, width: width, height: height), angle: 90)

// A soft teal oval behind the mascot (x 545-1375; the helper's confetti falls
// either side of it).
let halo = NSBezierPath(ovalIn: NSRect(x: 545, y: 305, width: 830, height: 640))
color(0.12, 0.37, 0.34, 0.36).setFill()
halo.fill()

// Faint vertical stripes for texture.
for index in 0..<12 {
    let x = CGFloat(100 + index * 160)
    color(0.14, 0.34, 0.38, 0.20).setFill()
    NSRect(x: x, y: 315, width: 3, height: 580).fill()
}

guard let mascot = NSImage(contentsOf: source) else {
    fatalError("Could not open snake mascot at \(source.path)")
}
// The snake mascot, horizontally centred, above the score panel area.
mascot.draw(in: NSRect(x: 670, y: 325, width: 580, height: 560),
            from: .zero, operation: .sourceOver, fraction: 1)

title("ALP SNAKE", y: 875, size: 170, fill: color(0.96, 0.88, 0.25))

// Cyan bars along the top and bottom edges.
color(0.04, 0.82, 0.83).setFill()
NSRect(x: 0, y: 0, width: width, height: 15).fill()
NSRect(x: 0, y: 1065, width: width, height: 15).fill()

// Optional preview of the score panel (same boxes the helper draws).
if CommandLine.arguments.contains("--sample-scores") {
    color(0.04, 0.11, 0.20).setFill()
    NSRect(x: 130, y: 50, width: 1660, height: 295).fill()
    color(0.97, 0.86, 0.14).setFill()
    NSRect(x: 130, y: 50, width: 1660, height: 8).fill()
    NSRect(x: 130, y: 337, width: 1660, height: 8).fill()
    color(0, 0.85, 0.96).setFill()
    NSRect(x: 957, y: 80, width: 6, height: 235).fill()
    label("SCORE", x: 350, y: 225, size: 68, fill: color(0.97, 0.86, 0.14))
    label("BEST", x: 1220, y: 225, size: 68, fill: color(0.97, 0.86, 0.14))
    label("0040", x: 312, y: 70, size: 142, fill: .white)
    label("0090", x: 1085, y: 70, size: 142, fill: color(0.24, 0.93, 0.56))
}

// Finish drawing and save as PNG.
NSGraphicsContext.restoreGraphicsState()
try bitmap.representation(using: .png, properties: [:])!.write(to: output)
