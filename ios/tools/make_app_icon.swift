// Draws the Glucose Ring app icon: 1024 x 1024, opaque sRGB PNG.
// A brushed-silver ring with a blood drop in the middle, on navy-to-teal.
//
// Regenerate (Command Line Tools are enough):
//   swiftc -O ios/tools/make_app_icon.swift -o /tmp/make_app_icon
//   /tmp/make_app_icon ios/GlucoseRing/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png
import CoreGraphics
import Foundation
import ImageIO

let px = 1024
let S = CGFloat(px)
let space = CGColorSpace(name: CGColorSpace.sRGB)!
// noneSkipLast: no alpha channel. iOS wants an opaque icon.
guard let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                          space: space, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
    fatalError("could not create a drawing context")
}

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

/// Stops are (colour, alpha, location).
func gradient(_ stops: [(UInt32, CGFloat, CGFloat)]) -> CGGradient {
    CGGradient(colorsSpace: space, colors: stops.map { rgb($0.0, $0.1) } as CFArray,
               locations: stops.map { $0.2 })!
}

let c = CGPoint(x: S / 2, y: S / 2)
func circle(_ r: CGFloat, at p: CGPoint = c) -> CGRect {
    CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)
}

// 1. Background: deep navy to teal, top-left to bottom-right (CG's origin is bottom-left).
ctx.drawLinearGradient(gradient([(0x0A1B3D, 1, 0), (0x0E5A6B, 1, 1)]),
                       start: CGPoint(x: 0, y: S), end: CGPoint(x: S, y: 0), options: [])

// 2. A soft teal glow behind the ring.
ctx.drawRadialGradient(gradient([(0x5FE3D0, 0.30, 0), (0x5FE3D0, 0, 1)]),
                       startCenter: c, startRadius: 0, endCenter: c, endRadius: S * 0.50, options: [])

// 3. The ring: a thick brushed-silver band, lifted by a soft shadow.
let outer = S * 0.345
let inner = S * 0.245
let band = CGMutablePath()
band.addEllipse(in: circle(outer))
band.addEllipse(in: circle(inner))
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -S * 0.012), blur: S * 0.035, color: rgb(0x000000, 0.45))
ctx.addPath(band)
ctx.setFillColor(rgb(0xC9D6E3))
ctx.fillPath(using: .evenOdd)
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(band)
ctx.clip(using: .evenOdd)
ctx.drawLinearGradient(gradient([(0xFFFFFF, 1, 0), (0xB7C6D6, 1, 0.45), (0xF2F6FA, 1, 0.62), (0x94A6BA, 1, 1)]),
                       start: CGPoint(x: c.x - outer, y: c.y + outer),
                       end: CGPoint(x: c.x + outer, y: c.y - outer), options: [])
ctx.restoreGState()

// 4. A blood drop in the middle, for glucose.
let r = S * 0.072                  // radius of the drop's round base
let baseY = c.y - r * 0.55         // centres the whole drop inside the ring
let tip = CGPoint(x: c.x, y: baseY + r * 2.15)
let drop = CGMutablePath()
drop.move(to: tip)
drop.addCurve(to: CGPoint(x: c.x + r, y: baseY),
              control1: CGPoint(x: c.x + r * 0.30, y: baseY + r * 1.45),
              control2: CGPoint(x: c.x + r, y: baseY + r * 0.80))
drop.addArc(center: CGPoint(x: c.x, y: baseY), radius: r, startAngle: 0, endAngle: .pi, clockwise: true)
drop.addCurve(to: tip,
              control1: CGPoint(x: c.x - r, y: baseY + r * 0.80),
              control2: CGPoint(x: c.x - r * 0.30, y: baseY + r * 1.45))
drop.closeSubpath()
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -S * 0.008), blur: S * 0.02, color: rgb(0x000000, 0.4))
ctx.addPath(drop)
ctx.setFillColor(rgb(0xE0303F))
ctx.fillPath()
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(drop)
ctx.clip()
ctx.drawLinearGradient(gradient([(0xFF6B6B, 1, 0), (0xC81D3A, 1, 1)]),
                       start: CGPoint(x: c.x - r, y: tip.y), end: CGPoint(x: c.x + r, y: baseY - r), options: [])
ctx.restoreGState()
ctx.setFillColor(rgb(0xFFFFFF, 0.55))   // highlight
ctx.fillEllipse(in: CGRect(x: c.x - r * 0.55, y: baseY + r * 0.05, width: r * 0.32, height: r * 0.55))

// 5. Write the PNG.
guard CommandLine.arguments.count == 2, let image = ctx.makeImage(),
      let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL,
                                                 "public.png" as CFString, 1, nil) else {
    fatalError("usage: make_app_icon <out.png>")
}
CGImageDestinationAddImage(dest, image, nil)
guard CGImageDestinationFinalize(dest) else { fatalError("PNG write failed") }
print("wrote \(CommandLine.arguments[1]) (\(px)x\(px), opaque)")
