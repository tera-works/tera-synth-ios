import SwiftUI
import UIKit

/// Hardware-synth palette (same as the Android app).
enum Hw {
    static let chassis = Color(red: 14 / 255, green: 15 / 255, blue: 17 / 255)
    static let panel = Color(red: 31 / 255, green: 33 / 255, blue: 37 / 255)
    static let edge = Color(red: 8 / 255, green: 9 / 255, blue: 10 / 255)
    static let silk = Color(red: 214 / 255, green: 218 / 255, blue: 226 / 255)
    static let engrave = Color(red: 160 / 255, green: 166 / 255, blue: 178 / 255)
    static let led = Color(red: 1, green: 140 / 255, blue: 26 / 255)
    static let ledOff = Color(red: 64 / 255, green: 40 / 255, blue: 24 / 255)
    static let lcdBg = Color(red: 12 / 255, green: 18 / 255, blue: 8 / 255)
    static let lcdFg = Color(red: 174 / 255, green: 234 / 255, blue: 0)
    static let lcdDim = Color(red: 96 / 255, green: 132 / 255, blue: 22 / 255)
    static let key = Color(red: 44 / 255, green: 47 / 255, blue: 53 / 255)
}

/// Framed panel with a silkscreen title.
struct Panel<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text(title.uppercased()).font(.system(size: 12, weight: .bold)).foregroundColor(Hw.silk)
                Rectangle().fill(Color(white: 0.35)).frame(height: 1)
            }
            content
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.panel))
        .overlay(RoundedRectangle(cornerRadius: 2).stroke(Hw.edge, lineWidth: 1))
    }
}

/// LCD read-out.
struct Lcd: View {
    let text: String
    var size: CGFloat = 16
    var dim = false
    var body: some View {
        Text(text)
            .font(.system(size: size, design: .monospaced))
            .foregroundColor(dim ? Hw.lcdDim : Hw.lcdFg)
            .lineLimit(1)
            .padding(.horizontal, 8).padding(.vertical, 4)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

/// Push button with an optional LED.
struct HwButton: View {
    let title: String
    var lit: Bool? = nil
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                if let lit {
                    Circle().fill(lit ? Hw.led : Hw.ledOff).frame(width: 7, height: 7)
                        .shadow(color: lit ? Hw.led : .clear, radius: 4)
                }
                Text(title).font(.system(size: 14, weight: .bold)).foregroundColor(.white)
            }
            .padding(.horizontal, 12).padding(.vertical, 9)
            .frame(minHeight: 40)
            .background(LinearGradient(colors: [Color(white: 0.27), Hw.key], startPoint: .top, endPoint: .bottom))
            .cornerRadius(3)
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(Hw.edge, lineWidth: 1))
        }
        .buttonStyle(.plain)
    }
}

/// Rotary knob with an LED ring: drag up / down, double tap for the default.
struct Knob: View {
    @EnvironmentObject var model: SynthModel
    let param: Int32
    var size: CGFloat = 52
    @State private var value: Float = 0
    @State private var start: Float = 0
    @State private var dragging = false

    var body: some View {
        let e = model.engine
        VStack(spacing: 3) {
            ZStack {
                ForEach(0..<15, id: \.self) { i in
                    let t = Float(i) / 14
                    let a = Angle.degrees(135 + 270 * Double(t))
                    Circle().fill(value > 0 && t <= value + 0.001 ? Hw.led : Hw.ledOff)
                        .frame(width: 3.4, height: 3.4)
                        .offset(x: cos(a.radians) * size / 2, y: sin(a.radians) * size / 2)
                }
                Circle().fill(LinearGradient(colors: [Color(white: 0.30), Color(white: 0.13)], startPoint: .top, endPoint: .bottom))
                    .frame(width: size - 12, height: size - 12)
                    .overlay(Circle().stroke(Color.black, lineWidth: 1))
                Rectangle().fill(Color.white).frame(width: 2.4, height: (size - 12) / 2 - 4)
                    .offset(y: -((size - 12) / 4 + 1))
                    .rotationEffect(.degrees(-135 + 270 * Double(value)))
            }
            .frame(width: size + 8, height: size + 8)
            .contentShape(Rectangle())
            .overlay(Circle().stroke(model.learnTarget == param ? Hw.led : Color.clear, lineWidth: 2).padding(2))
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { g in
                    if model.learnMode { return }  // learn mode: a tap picks the knob (onEnded)
                    if !dragging {
                        dragging = true
                        start = value
                        model.checkpoint()
                    }
                    let t = min(1, max(0, start + Float(-g.translation.height + g.translation.width * 0.5) / 180))
                    value = t
                    e.setParam(param, normalized: t)
                    model.show("\(e.paramLabel(param))  \(e.formatParam(param))")
                }
                .onEnded { _ in
                    if model.learnMode { model.learn(param) }
                    dragging = false
                })
            .onTapGesture(count: 2) {
                if model.learnMode { return }
                model.checkpoint()
                e.setParam(param, value: e.paramDefault(param))
                value = e.paramNormalized(param)
            }
            Text(e.paramLabel(param).uppercased()).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave).lineLimit(1)
            Text(e.formatParam(param)).font(.system(size: 10.5, design: .monospaced)).foregroundColor(Hw.lcdFg).lineLimit(1)
            if model.learnMode {
                let cc = e.learnDescribe(param)
                Text(cc.isEmpty ? "—" : cc).font(.system(size: 9.5, design: .monospaced)).foregroundColor(Hw.led).lineLimit(1)
            }
        }
        .frame(width: max(size + 12, 64))
        .onAppear { value = e.paramNormalized(param) }
        .onChange(of: model.revision) { _ in value = e.paramNormalized(param) }
    }
}

/// One parameter as a control: knob, enum menu or on / off switch with LED.
struct ParamControl: View {
    @EnvironmentObject var model: SynthModel
    let param: Int32
    @State private var refresh = 0
    var body: some View {
        let e = model.engine
        switch e.paramKind(param) {
        case 2:
            VStack(alignment: .leading, spacing: 4) {
                Text(e.paramLabel(param).uppercased()).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                Menu {
                    ForEach(Array(e.paramOptions(param).enumerated()), id: \.offset) { i, o in
                        Button(o) {
                            model.checkpoint()
                            e.setParam(param, value: Float(i))
                            refresh += 1
                            model.show("\(e.paramLabel(param))  \(o)")
                        }
                    }
                } label: { Lcd(text: e.formatParam(param) + " ▾", size: 13).frame(width: 120) }
            }
            .id(refresh &+ model.revision)
        case 3:
            VStack(spacing: 4) {
                let on = e.paramValue(param) > 0.5
                Circle().fill(on ? Hw.led : Hw.ledOff).frame(width: 8, height: 8).shadow(color: on ? Hw.led : .clear, radius: 4)
                Toggle("", isOn: Binding(get: { e.paramValue(param) > 0.5 },
                                         set: { model.checkpoint(); e.setParam(param, value: $0 ? 1 : 0); refresh += 1 }))
                    .labelsHidden().tint(Hw.led)
                Text(e.paramLabel(param).uppercased()).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
            }
            .id(refresh &+ model.revision)
        default:
            Knob(param: param)
        }
    }
}

/// Pitch / mod wheel strip.
struct Wheel: View {
    let title: String
    let spring: Bool
    let onChange: (Float) -> Void
    @State private var value: Float = 0
    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .top) {
                RoundedRectangle(cornerRadius: 4).fill(LinearGradient(colors: [Color(white: 0.08), Color(white: 0.27), Color(white: 0.08)],
                                                                      startPoint: .top, endPoint: .bottom))
                let h = g.size.height
                let y = spring ? h / 2 - CGFloat(value) * (h / 2 - 8) : h - 8 - CGFloat(value) * (h - 16)
                RoundedRectangle(cornerRadius: 2).fill(Hw.led).frame(height: 6).offset(y: y - 3).padding(.horizontal, 4)
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { d in
                    let t = Float(1 - min(1, max(0, d.location.y / g.size.height)))
                    value = spring ? t * 2 - 1 : t
                    onChange(value)
                }
                .onEnded { _ in if spring { value = 0; onChange(0) } })
        }
        .overlay(alignment: .top) { Text(title).font(.system(size: 11, weight: .bold)).foregroundColor(Hw.engrave).offset(y: -16) }
    }
}

// ---------------------------------------------------------------- keyboard

/// Multi-touch piano keyboard (UIKit, so every finger is tracked).
struct KeyboardView: UIViewRepresentable {
    let engine: HSynthEngine
    @Binding var firstNote: Int
    @Binding var whiteKeys: Int
    func makeUIView(context: Context) -> KeyboardUIView {
        let v = KeyboardUIView()
        v.engine = engine
        return v
    }
    func updateUIView(_ v: KeyboardUIView, context: Context) {
        v.firstNote = firstNote
        v.whiteKeys = whiteKeys
        v.setNeedsDisplay()
    }
}

final class KeyboardUIView: UIView {
    var engine: HSynthEngine?
    var firstNote = 48 { didSet { if oldValue != firstNote { allOff() } } }
    var whiteKeys = 15 { didSet { if oldValue != whiteKeys { allOff() } } }
    private var touchKeys: [ObjectIdentifier: Int] = [:]
    private var held = [Int](repeating: 0, count: 128)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .black
        contentMode = .redraw
    }
    required init?(coder: NSCoder) { fatalError() }

    private func isBlack(_ n: Int) -> Bool { [1, 3, 6, 8, 10].contains(n % 12) }
    private func whiteNote(_ i: Int) -> Int {
        var n = firstNote, k = 0
        while k < i { n += 1; if !isBlack(n) { k += 1 } }
        return n
    }
    private func note(at p: CGPoint) -> Int {
        let ww = bounds.width / CGFloat(whiteKeys)
        let wi = min(whiteKeys - 1, max(0, Int(p.x / ww)))
        let wn = whiteNote(wi)
        if p.y < bounds.height * 0.62 {
            let local = p.x - CGFloat(wi) * ww
            if local > ww * 0.68, isBlack(wn + 1), wi < whiteKeys - 1 { return wn + 1 }
            if local < ww * 0.32, wn - 1 >= firstNote, isBlack(wn - 1) { return wn - 1 }
        }
        return wn
    }
    private func velocity(_ p: CGPoint, _ n: Int) -> Float {
        let h = isBlack(n) ? bounds.height * 0.62 : bounds.height
        return Float(min(1, max(0.05, 0.25 + 0.75 * p.y / h)))
    }

    override func draw(_ rect: CGRect) {
        guard let c = UIGraphicsGetCurrentContext() else { return }
        let ww = bounds.width / CGFloat(whiteKeys), h = bounds.height
        for i in 0..<whiteKeys {
            let n = whiteNote(i)
            let r = CGRect(x: CGFloat(i) * ww, y: 0, width: ww, height: h)
            c.setFillColor(held[n] > 0 ? UIColor(red: 1, green: 0.77, blue: 0.55, alpha: 1).cgColor : UIColor(white: 0.93, alpha: 1).cgColor)
            c.fill(r)
            c.setStrokeColor(UIColor(white: 0.25, alpha: 1).cgColor)
            c.stroke(r)
            if n % 12 == 0 {
                ("C\(n / 12 - 1)" as NSString).draw(at: CGPoint(x: r.minX + 4, y: h - 18),
                                                     withAttributes: [.font: UIFont.systemFont(ofSize: 11), .foregroundColor: UIColor.darkGray])
            }
        }
        for i in 0..<(whiteKeys - 1) {
            let n = whiteNote(i)
            guard isBlack(n + 1) else { continue }
            let x = CGFloat(i + 1) * ww
            c.setFillColor(held[n + 1] > 0 ? UIColor(red: 0.9, green: 0.47, blue: 0.12, alpha: 1).cgColor : UIColor(white: 0.1, alpha: 1).cgColor)
            c.fill(CGRect(x: x - ww * 0.32, y: 0, width: ww * 0.64, height: h * 0.62))
        }
    }

    private func press(_ t: UITouch) {
        let p = t.location(in: self)
        let n = note(at: p)
        touchKeys[ObjectIdentifier(t)] = n
        held[n] += 1
        engine?.noteOn(Int32(n), velocity: velocity(p, n))
        setNeedsDisplay()
    }
    private func release(_ t: UITouch) {
        guard let n = touchKeys.removeValue(forKey: ObjectIdentifier(t)) else { return }
        held[n] = max(0, held[n] - 1)
        if held[n] == 0 { engine?.noteOff(Int32(n)) }
        setNeedsDisplay()
    }
    func allOff() {
        for (_, n) in touchKeys { engine?.noteOff(Int32(n)) }
        touchKeys.removeAll()
        held = [Int](repeating: 0, count: 128)
        setNeedsDisplay()
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { touches.forEach(press) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {   // glissando: a new key under the finger retriggers
            if let old = touchKeys[ObjectIdentifier(t)], note(at: t.location(in: self)) != old { release(t); press(t) }
        }
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { touches.forEach(release) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { touches.forEach(release) }
}
