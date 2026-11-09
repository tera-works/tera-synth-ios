import SwiftUI

// Performance features: the Super Knob with scale lock / chord mode, the phrase
// recorder (live loop: record, overdub, quantize, export) and sampling (record
// the input or the output, trim, loop, save as an instrument).

// ---------------------------------------------------------------- perform

/// Super Knob (one for every part: route "super" in a mod matrix), scale lock and chord mode.
struct PerformPanel: View {
    @EnvironmentObject var model: SynthModel
    private let names = ["scale.lock", "scale.root", "scale.type", "chord.mode", "chord.inversion"]

    var body: some View {
        let e = model.engine
        Panel(title: "Perform") {
            HStack(alignment: .top, spacing: 6) {
                let superId = e.paramId("perf.super")
                if superId >= 0 { Knob(param: superId, size: 64) }
                ForEach(names, id: \.self) { n in
                    let pid = e.paramId(n)
                    if pid >= 0 { ParamControl(param: pid) }
                }
            }
        }
    }
}

// ---------------------------------------------------------------- phrase recorder

/// Record / Play / Stop / Undo / Clear, length, quantize and export of the phrase recorder.
struct PhrasePanel: View {
    @EnvironmentObject var model: SynthModel
    @State private var state: Int32 = 0
    @State private var pos = 0.0
    @State private var len = 0.0
    @State private var count: Int32 = 0
    @State private var bars: Int32 = 0
    @State private var message = ""
    private let timer = Timer.publish(every: 0.1, on: .main, in: .common).autoconnect()

    private var stateName: String {
        switch state {
        case 1: return "REC"
        case 2: return "PLAY"
        case 3: return "OVERDUB"
        case 4: return "STOP"
        default: return "EMPTY"
        }
    }

    var body: some View {
        let e = model.engine
        Panel(title: "Phrase") {
            HStack(spacing: 6) {
                HwButton(title: "●", lit: state == 1 || state == 3) { e.phraseRecord() }
                HwButton(title: "▶", lit: state == 2) { e.phrasePlay() }
                HwButton(title: "■") { e.phraseStop() }
                HwButton(title: "Undo") { e.phraseUndo() }
                HwButton(title: "Clear") { e.phraseClear() }
            }
            HStack(spacing: 6) {
                Lcd(text: len > 0 ? String(format: "%@  %.1f / %.0f  %d", stateName, pos, len, count)
                                  : String(format: "%@  %.1f  %d", stateName, pos, count), size: 13)
                    .frame(width: 170)
                Menu {
                    ForEach([0, 1, 2, 4, 8], id: \.self) { b in
                        Button(b == 0 ? "Free" : "\(b) bar\(b > 1 ? "s" : "")") {
                            e.phraseBars = Int32(b)
                            bars = Int32(b)
                        }
                    }
                } label: {
                    Lcd(text: bars == 0 ? "LEN Free ▾" : "LEN \(bars) ▾", size: 13).frame(width: 96)
                }
                Menu {
                    ForEach([("1/4", 1.0), ("1/8", 0.5), ("1/16", 0.25), ("1/32", 0.125), ("1/8T", 1.0 / 3.0)], id: \.0) { t, g in
                        Button(t) { message = e.phraseQuantize(g) ? "Quantized to \(t)" : "Quantize: start the audio first" }
                    }
                } label: {
                    Lcd(text: "QUANT ▾", size: 13).frame(width: 90)
                }
                HwButton(title: "Save .mid") { export() }
            }
            if !message.isEmpty {
                Text(message).font(.system(size: 11)).foregroundColor(Hw.lcdFg)
            }
        }
        .onAppear { bars = e.phraseBars }
        .onReceive(timer) { _ in
            state = e.phraseState
            pos = e.phrasePosition
            len = e.phraseLength
            count = e.phraseEventCount
        }
    }

    /// The phrase as a MIDI file in Documents/midi (it shows up in the Player).
    private func export() {
        let e = model.engine
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        let path = (e.midiDir as NSString).appendingPathComponent("Phrase \(f.string(from: Date())).mid")
        if let err = e.phraseExport(path, repeats: 4) {
            message = "Cannot save: \(err)"
        } else {
            message = "Saved \((path as NSString).lastPathComponent) (4 times round) in the Player's songs"
            model.player.refresh()
        }
    }
}

// ---------------------------------------------------------------- sampling

/// Record the audio input (or HSynth's own output), mark the part to keep, loop it, save it
/// as an instrument and play it in a layer of the edit part.
struct SampleScreen: View {
    @EnvironmentObject var model: SynthModel
    @State private var fromInput = true
    @State private var seconds = -1.0
    @State private var wave: [Float] = []
    @State private var frames: Int64 = 0
    @State private var start: Int64 = 0
    @State private var end: Int64 = 0
    @State private var loopOn = false
    @State private var loopStart: Int64 = 0
    @State private var loopEnd: Int64 = 0
    @State private var normalize = true
    @State private var root = 60
    @State private var name = "My Sample"
    @State private var message = ""
    @State private var dragHandle = -1
    private let timer = Timer.publish(every: 0.1, on: .main, in: .common).autoconnect()

    private static let notes = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    private func noteName(_ n: Int) -> String { Self.notes[((n % 12) + 12) % 12] + "\(n / 12 - 1)" }

    var body: some View {
        let e = model.engine
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Panel(title: "Record") {
                    HStack(spacing: 8) {
                        HwButton(title: "Input", lit: fromInput) { fromInput = true }
                        HwButton(title: "Output", lit: !fromInput) { fromInput = false }
                        HwButton(title: seconds >= 0 ? "■ Stop" : "● Rec", lit: seconds >= 0) { toggle() }
                        Lcd(text: seconds >= 0 ? String(format: "REC %.1f s", seconds)
                                               : (frames > 0 ? String(format: "TAKE %.2f s", Double(frames) / Double(e.takeRate)) : "—"),
                            size: 14)
                    }
                    Text("Input はマイクや接続したオーディオ入力、Output は HSynth 自身の音を録ります（最大 30 秒）。Input を初めて使うときはマイクの許可を求めます。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "Edit") {
                    waveform.frame(height: 130)
                    HStack(spacing: 8) {
                        HwButton(title: "Auto Trim") { autoTrim() }
                        HwButton(title: "Loop", lit: loopOn) { loopOn.toggle() }
                        HwButton(title: "Find Loop") { findLoop() }
                        HwButton(title: "Normalize", lit: normalize) { normalize.toggle() }
                        HStack(spacing: 3) {
                            Text("ROOT").font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                            HwButton(title: "◀") { root = max(12, root - 1) }
                            Lcd(text: noteName(root), size: 14).frame(width: 56)
                            HwButton(title: "▶") { root = min(108, root + 1) }
                        }
                    }
                    Text("緑の線が始まり、赤の線が終わり、オレンジの線がループの範囲です。線の近くをドラッグして動かします。ROOT は録った音の高さ（その鍵盤で元の高さで鳴ります）。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "Save") {
                    HStack(spacing: 8) {
                        TextField("Name", text: $name)
                            .textFieldStyle(.plain).font(.system(size: 15, design: .monospaced)).foregroundColor(Hw.lcdFg)
                            .padding(8).background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg)).frame(width: 220)
                        ForEach(0..<3, id: \.self) { l in
                            HwButton(title: "Save → Layer \(["A", "B", "C"][l])") { save(layer: Int32(l)) }
                        }
                    }
                    Text("library/user/sampled に WAV と SFZ で保存し、エディット中のパートの層に読み込みます。層の種類を Granular にすると、グラニュラーの素材にもなります。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                    if !message.isEmpty {
                        Text(message).font(.system(size: 12)).foregroundColor(Hw.lcdFg)
                    }
                }
            }
            .padding(8)
        }
        .onReceive(timer) { _ in seconds = e.samplingSeconds }
    }

    private var waveform: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            let total = max(1.0, Double(frames))
            let xOf = { (f: Int64) -> CGFloat in CGFloat(Double(f) / total) * w }
            Canvas { c, _ in
                if !wave.isEmpty {
                    let bw = w / CGFloat(wave.count)
                    for (i, v) in wave.enumerated() {
                        let bh = CGFloat(v) * (h - 8)
                        c.fill(Path(CGRect(x: CGFloat(i) * bw, y: (h - bh) / 2, width: max(1, bw - 0.5), height: bh)),
                               with: .color(Hw.lcdFg.opacity(0.8)))
                    }
                }
                if frames > 0 {
                    c.fill(Path(CGRect(x: 0, y: 0, width: xOf(start), height: h)), with: .color(Color.black.opacity(0.55)))
                    c.fill(Path(CGRect(x: xOf(end), y: 0, width: w - xOf(end), height: h)), with: .color(Color.black.opacity(0.55)))
                    if loopOn && loopEnd > loopStart {
                        c.fill(Path(CGRect(x: xOf(loopStart), y: 0, width: xOf(loopEnd) - xOf(loopStart), height: h)),
                               with: .color(Hw.led.opacity(0.18)))
                        for f in [loopStart, loopEnd] {
                            c.fill(Path(CGRect(x: xOf(f) - 1, y: 0, width: 2, height: h)), with: .color(Hw.led))
                        }
                    }
                    c.fill(Path(CGRect(x: xOf(start) - 1, y: 0, width: 2, height: h)), with: .color(.green))
                    c.fill(Path(CGRect(x: xOf(end) - 1, y: 0, width: 2, height: h)), with: .color(.red))
                }
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { d in
                    guard frames > 0 else { return }
                    let f = Int64(max(0, min(1, d.location.x / w)) * total)
                    if dragHandle < 0 {
                        var handles: [(Int, Int64)] = [(0, start), (1, end)]
                        if loopOn { handles += [(2, loopStart), (3, loopEnd)] }
                        let x0 = d.startLocation.x
                        dragHandle = handles.min { abs(xOf($0.1) - x0) < abs(xOf($1.1) - x0) }?.0 ?? 0
                    }
                    switch dragHandle {
                    case 0: start = min(f, end - 64)
                    case 1: end = max(f, start + 64)
                    case 2: loopStart = max(start, min(f, loopEnd - 64))
                    default: loopEnd = min(end, max(f, loopStart + 64))
                    }
                }
                .onEnded { _ in dragHandle = -1 })
        }
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }

    private func toggle() {
        let e = model.engine
        if e.samplingSeconds >= 0 {
            if e.stopSampling() {
                frames = e.takeFrames
                wave = e.takeWaveform(240).map { $0.floatValue }
                autoTrim()
                loopOn = false
                message = ""
            } else {
                message = "Nothing was recorded."
            }
            seconds = -1
        } else {
            model.start()
            if fromInput && !e.audioInputOn { e.setAudioInput(true) }
            e.startSampling(fromInput)
            seconds = 0
        }
    }

    private func autoTrim() {
        let b = model.engine.takeBounds().map { $0.int64Value }
        if b.count == 2 {
            start = b[0]
            end = b[1]
        }
    }

    private func findLoop() {
        if let l = model.engine.takeFindLoop(from: start, to: end)?.map({ $0.int64Value }), l.count == 2 {
            loopStart = l[0]
            loopEnd = l[1]
            loopOn = true
            message = "Loop found."
        } else {
            message = "No seamless loop: a steady part of at least half a second is needed."
        }
    }

    private func save(layer: Int32) {
        guard frames > 0 else {
            message = "Record something first."
            return
        }
        model.checkpoint()
        if let err = model.engine.saveTake(name, start: start, end: end, normalize: normalize,
                                           loopStart: loopOn ? loopStart : 0, loopEnd: loopOn ? loopEnd : 0,
                                           rootKey: Int32(root), layer: layer) {
            message = "Cannot save: \(err)"
        } else {
            model.revision += 1
            message = "Saved and loaded into layer \(["A", "B", "C"][Int(layer)]). Play the keyboard on the Play screen."
        }
    }
}
