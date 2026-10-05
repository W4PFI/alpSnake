// make_smooth_font.swift - pre-renders the anti-aliased font atlas
// assets/snake-font.bin.
//
// Usage: swift make_smooth_font.swift <Nunito-wght.ttf> <snake-font.bin>
//
// The cabinet-side code has no font engine (no FreeType, no libc), so text is
// rendered once here on the Mac and shipped as raw pixels. The game core and
// the backglass helper read the file with src/snake_smooth_font.c and scale and
// tint the glyphs themselves. This is a one-off tool: build.py does not run it,
// it only checks the atlas it produced.
//
// File format (4 + 37 * 128 * 144 bytes):
//   "SNK1"                       4-byte magic/version
//   37 glyphs, in the order A-Z, 0-9, space; each is 128 x 144 bytes, one
//   8-bit coverage (alpha) value per pixel, 0 = empty, 255 = solid, row by row.
// Each character is drawn in Nunito ExtraBold at 112 points, horizontally
// centred in its cell, white on transparent; only the alpha is kept.
import AppKit
import CoreText
import Foundation

let fontURL = URL(fileURLWithPath: CommandLine.arguments[1])
let outputURL = URL(fileURLWithPath: CommandLine.arguments[2])
guard CTFontManagerRegisterFontsForURL(fontURL as CFURL, .process, nil),
      let font = NSFont(name: "Nunito-ExtraBold", size: 112) else {
    fatalError("Could not load Nunito from \(fontURL.path)")
}

// Glyph order matters: the readers index the atlas by this order.
let glyphs = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ")
// Atlas cell size (SNAKE_FONT_WIDTH x SNAKE_FONT_HEIGHT in snake_smooth_font.h).
let width = 128
let height = 144
var data = Data("SNK1".utf8)

for glyph in glyphs {
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
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    NSGraphicsContext.current?.imageInterpolation = .high
    let string = String(glyph) as NSString
    let attributes: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor.white
    ]
    // Centre horizontally; draw 8 pixels up from the cell's bottom edge.
    let x = (CGFloat(width) - string.size(withAttributes: attributes).width) / 2
    string.draw(at: NSPoint(x: x, y: 8), withAttributes: attributes)
    NSGraphicsContext.restoreGraphicsState()
    // Append the alpha channel. colorAt(x:y:) counts y from the TOP row, so
    // rows are stored top to bottom, as the C readers expect.
    for row in 0..<height {
        for column in 0..<width {
            let alpha = bitmap.colorAt(x: column, y: row)?.alphaComponent ?? 0
            data.append(UInt8((alpha * 255).rounded()))
        }
    }
}

try data.write(to: outputURL)
