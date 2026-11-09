import SwiftUI
import UniformTypeIdentifiers

// Screens and editor parts ported from the Android app: envelope / filter
// graphs, sample loading, Multi (16 parts), the preset picker and System.

// ---------------------------------------------------------------- graphs

/// ADSR of "amp" or "env2" (times on a square-root scale so short and long both show).
struct EnvGraph: View {
    @EnvironmentObject var model: SynthModel
    let prefix: String

    var body: some View {
        let e = model.engine
        let ids = ["attack", "decay", "sustain", "release"].map { e.paramId(prefix + "." + $0) }
        TimelineView(.periodic(from: .now, by: 0.1)) { _ in
            Canvas { c, size in
                guard ids.allSatisfy({ $0 >= 0 }) else { return }
                let a = Double(e.paramValue(ids[0])), d = Double(e.paramValue(ids[1]))
                let s = Double(e.paramValue(ids[2])), r = Double(e.paramValue(ids[3]))
                let wa = (a / 10).squareRoot(), wd = (d / 20).squareRoot(), ws = 0.35, wr = (r / 20).squareRoot()
                let total = max(0.001, wa + wd + ws + wr)
                let w = size.width - 8, h = size.height - 8
                let x0: CGFloat = 4, y0: CGFloat = 4
                func x(_ t: Double) -> CGFloat { x0 + CGFloat(t / total) * w }
                let sy = y0 + CGFloat(1 - s) * h
                var p = Path()
                p.move(to: CGPoint(x: x0, y: y0 + h))
                p.addLine(to: CGPoint(x: x(wa), y: y0))
                p.addLine(to: CGPoint(x: x(wa + wd), y: sy))
                p.addLine(to: CGPoint(x: x(wa + wd + ws), y: sy))
                p.addLine(to: CGPoint(x: x(total), y: y0 + h))
                var fill = p
                fill.closeSubpath()
                c.fill(fill, with: .color(Hw.lcdFg.opacity(0.15)))
                c.stroke(p, with: .color(Hw.lcdFg), lineWidth: 2)
                for t in [wa, wa + wd, wa + wd + ws] {
                    var g = Path()
                    g.move(to: CGPoint(x: x(t), y: y0))
                    g.addLine(to: CGPoint(x: x(t), y: y0 + h))
                    c.stroke(g, with: .color(Hw.lcdDim.opacity(0.5)), lineWidth: 1)
                }
            }
        }
        .frame(height: 84)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

/// Approximate response of the filter (type, cutoff, resonance), 20 Hz .. 20 kHz, -48 .. +24 dB.
struct FilterGraph: View {
    @EnvironmentObject var model: SynthModel

    var body: some View {
        let e = model.engine
        let typeId = e.paramId("filter.type"), cutId = e.paramId("filter.cutoff"), resId = e.paramId("filter.reso")
        TimelineView(.periodic(from: .now, by: 0.1)) { _ in
            Canvas { c, size in
                guard typeId >= 0, cutId >= 0, resId >= 0 else { return }
                let type = e.formatParam(typeId).uppercased()
                let fc = Double(max(20, e.paramValue(cutId)))
                let q = 0.707 + Double(e.paramValue(resId)) * 9
                let w = size.width, h = size.height
                func y(_ db: Double) -> CGFloat { CGFloat((24 - min(24, max(-48, db))) / 72) * h }
                var zero = Path()
                zero.move(to: CGPoint(x: 0, y: y(0)))
                zero.addLine(to: CGPoint(x: w, y: y(0)))
                c.stroke(zero, with: .color(Hw.lcdDim.opacity(0.5)), lineWidth: 1)
                var p = Path()
                let n = 120
                for i in 0...n {
                    let f = 20 * pow(1000.0, Double(i) / Double(n))
                    let r = f / fc
                    let den = ((1 - r * r) * (1 - r * r) + (r / q) * (r / q)).squareRoot()
                    var mag: Double
                    if type.contains("OFF") { mag = 1 }
                    else if type.contains("HP") { mag = r * r / den }
                    else if type.contains("BP") { mag = (r / q) / den }
                    else if type.contains("NOTCH") || type.contains("BR") { mag = abs(1 - r * r) / den }
                    else { mag = 1 / den }
                    if type.contains("24") && !type.contains("BP") { mag *= mag }
                    let pt = CGPoint(x: CGFloat(i) / CGFloat(n) * w, y: y(20 * log10(max(1e-6, mag))))
                    if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
                }
                c.stroke(p, with: .color(Hw.lcdFg), lineWidth: 2)
            }
        }
        .frame(height: 84)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

// ---------------------------------------------------------------- samples

/// The sample sets of the edit part's layers: load from the Files app, clear.
struct SamplesPanel: View {
    @EnvironmentObject var model: SynthModel
    @State private var importing = false
    @State private var layer: Int32 = 0
    @State private var message = ""

    private var types: [UTType] {
        var t: [UTType] = [.folder, .audio, .wav]
        if let sfz = UTType(filenameExtension: "sfz") { t.append(sfz) }
        if let flac = UTType(filenameExtension: "flac") { t.append(flac) }
        return t
    }

    private func setName(_ l: Int) -> String {
        let n = model.engine.sampleSetName(Int32(l))
        return n.isEmpty ? "(none)" : n
    }

    var body: some View {
        let e = model.engine
        Panel(title: "Samples") {
            ForEach(0..<3, id: \.self) { l in
                HStack(spacing: 8) {
                    Text(["A", "B", "C"][l]).font(.system(size: 13, weight: .bold)).foregroundColor(Hw.silk).frame(width: 16)
                    Lcd(text: setName(l), size: 13)
                    HwButton(title: "Load…") {
                        layer = Int32(l)
                        importing = true
                    }
                    HwButton(title: "Clear") {
                        model.checkpoint()
                        e.unloadSamples(Int32(l))
                        model.revision += 1
                    }
                }
            }
            if !message.isEmpty {
                Text(message).font(.system(size: 12)).foregroundColor(Hw.lcdFg)
            }
            Text("SFZ は、サンプルの入ったフォルダごと選んでください。HSynth のフォルダ（library）の中のファイルはそのまま、ほかの場所のものは library/user/imported にコピーして使います。")
                .font(.system(size: 11)).foregroundColor(Hw.engrave).fixedSize(horizontal: false, vertical: true)
        }
        .id(model.revision)
        .fileImporter(isPresented: $importing, allowedContentTypes: types, allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            message = model.importSamples(url, layer: layer)
        }
    }
}

extension SynthModel {
    /// Copies a picked file / folder into Documents/library/user/imported (unless it is in the
    /// library already) and loads it into a layer of the edit part. Returns a message for the user.
    func importSamples(_ url: URL, layer: Int32) -> String {
        let fm = FileManager.default
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let lib = URL(fileURLWithPath: engine.libraryDir).resolvingSymlinksInPath().standardizedFileURL
        var target = url.resolvingSymlinksInPath().standardizedFileURL
        if !target.path.hasPrefix(lib.path + "/") {
            let dir = lib.appendingPathComponent("user/imported", isDirectory: true)
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
            let dest = dir.appendingPathComponent(url.lastPathComponent)
            try? fm.removeItem(at: dest)
            do {
                try fm.copyItem(at: url, to: dest)
            } catch {
                return "コピーできませんでした: \(error.localizedDescription)"
            }
            target = dest
        }
        var isDir: ObjCBool = false
        if fm.fileExists(atPath: target.path, isDirectory: &isDir), isDir.boolValue {
            guard let file = Self.firstFile(in: target, ext: "sfz") ?? Self.firstFile(in: target, ext: "wav")
                    ?? Self.firstFile(in: target, ext: "flac") else {
                return "フォルダに SFZ / WAV / FLAC のファイルがありません。"
            }
            target = file
        }
        checkpoint()
        if let err = engine.loadSamples(target.path, layer: layer) {
            return "読み込めませんでした: \(err)"
        }
        revision += 1
        let warnings = engine.lastWarnings
        return warnings.isEmpty ? "\(target.lastPathComponent) を読み込みました。"
                                : "\(target.lastPathComponent) を読み込みました（注意 \(warnings.count) 件: \(warnings[0])）"
    }

    static func firstFile(in dir: URL, ext: String) -> URL? {
        guard let it = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil) else { return nil }
        while let u = it.nextObject() as? URL {
            if u.pathExtension.lowercased() == ext { return u }
        }
        return nil
    }
}

// ---------------------------------------------------------------- presets

/// Searchable preset list in a sheet.
struct PresetPicker: View {
    @EnvironmentObject var model: SynthModel
    @Environment(\.dismiss) private var dismiss
    let title: String
    let onPick: (Int) -> Void
    @State private var search = ""

    var body: some View {
        let names = model.engine.presetNames
        let cats = model.engine.presetCategories
        let shown = names.indices.filter { i in
            search.isEmpty || (names[i] + " " + cats[i]).localizedCaseInsensitiveContains(search)
        }
        VStack(spacing: 8) {
            HStack {
                Text(title).font(.system(size: 16, weight: .bold)).foregroundColor(.white)
                Spacer()
                HwButton(title: "Close") { dismiss() }
            }
            TextField("Search", text: $search)
                .textFieldStyle(.plain).font(.system(size: 15, design: .monospaced)).foregroundColor(Hw.lcdFg)
                .padding(8).background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
            List(shown, id: \.self) { i in
                Button {
                    onPick(i)
                    dismiss()
                } label: {
                    HStack {
                        Text(names[i]).foregroundColor(.white)
                        Spacer()
                        Text(cats[i]).font(.system(size: 12)).foregroundColor(Hw.engrave)
                    }
                }
                .listRowBackground(Hw.panel)
            }
            .listStyle(.plain)
        }
        .padding(12)
        .background(Hw.chassis.ignoresSafeArea())
    }
}

// ---------------------------------------------------------------- multi

/// Multi: 16 parts (sound, MIDI channel, level, pan, mute, key range, transpose)
/// and the part the editor works on. Parts on one channel layer, key ranges split.
struct MultiScreen: View {
    @EnvironmentObject var model: SynthModel
    @State private var picking = false
    @State private var pickPart = 0

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 10) {
                HwButton(title: "Multi Mode", lit: model.multi) { model.setMulti(!model.multi) }
                Text("オンにすると 16 パートを同時に鳴らせます。同じ MIDI チャンネルのパートは重なり、鍵盤の範囲を分けるとスプリットになります。番号のボタンで、Edit 画面で編集するパートを選びます。")
                    .font(.system(size: 11)).foregroundColor(Hw.engrave).fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 8)
            ScrollView(.horizontal, showsIndicators: false) {
                SceneBar().padding(.horizontal, 8)
            }
            ScrollView([.vertical, .horizontal]) {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        ForEach([("PART", 64), ("SOUND", 170), ("CH", 104), ("LEVEL", 96), ("PAN", 76), ("", 44),
                                 ("KEY LOW", 116), ("KEY HIGH", 116), ("TRANSPOSE", 104)], id: \.0) { t, w in
                            Text(t).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                                .frame(width: CGFloat(w), alignment: .leading)
                        }
                    }
                    ForEach(0..<16, id: \.self) { k in
                        PartRow(part: k) {
                            pickPart = k
                            picking = true
                        }
                    }
                }
                .padding(.horizontal, 8)
                .id(model.revision)
            }
        }
        .padding(.top, 8)
        .sheet(isPresented: $picking) {
            PresetPicker(title: "Part \(pickPart + 1)") { i in
                if !model.multi { model.setMulti(true) }
                model.selectPart(pickPart)
                model.load(preset: i)
            }
            .environmentObject(model)
        }
    }
}

struct PartRow: View {
    @EnvironmentObject var model: SynthModel
    let part: Int
    let pick: () -> Void
    @State private var tick = 0

    private static let noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    private static func note(_ v: Float) -> String {
        let n = Int(v.rounded())
        return noteNames[((n % 12) + 12) % 12] + "\(n / 12 - 1)"
    }

    var body: some View {
        let e = model.engine
        let k = Int32(part)
        let muted = e.partMix(k, id: 2) > 0.5
        HStack(spacing: 6) {
            HwButton(title: "\(part + 1)", lit: model.editPart == part) { model.selectPart(part) }
                .frame(width: 64, alignment: .leading)
            Button(action: pick) {
                Lcd(text: model.partNames[part].isEmpty ? "Init" : model.partNames[part], size: 13)
            }
            .buttonStyle(.plain)
            .frame(width: 170)
            stepper(id: 3, lo: 1, hi: 16, width: 104) { "\(Int($0))" }
            Slider(value: Binding(get: { Double(e.partMix(k, id: 0)) },
                                  set: { e.setPartMix(k, id: 0, value: Float($0)); tick += 1 }), in: 0...1)
                .tint(Hw.led).frame(width: 96)
            Slider(value: Binding(get: { Double(e.partMix(k, id: 1)) },
                                  set: { e.setPartMix(k, id: 1, value: Float($0)); tick += 1 }), in: -1...1)
                .tint(Hw.led).frame(width: 76)
            HwButton(title: "M", lit: muted) {
                e.setPartMix(k, id: 2, value: muted ? 0 : 1)
                tick += 1
            }
            .frame(width: 44)
            stepper(id: 4, lo: 0, hi: 127, width: 116) { Self.note($0) }
            stepper(id: 5, lo: 0, hi: 127, width: 116) { Self.note($0) }
            stepper(id: 6, lo: -48, hi: 48, width: 104) { v in v > 0 ? "+\(Int(v))" : "\(Int(v))" }
        }
        .padding(.vertical, 2)
        .background(part % 2 == 0 ? Color(white: 0.09) : Color.clear)
        .opacity(model.multi || part == 0 ? 1 : 0.45)
        .id(tick)
    }

    private func stepper(id: Int32, lo: Float, hi: Float, width: CGFloat, format: @escaping (Float) -> String) -> some View {
        let e = model.engine
        let k = Int32(part)
        let v = e.partMix(k, id: id)
        return HStack(spacing: 2) {
            Button {
                e.setPartMix(k, id: id, value: max(lo, v - 1))
                tick += 1
            } label: {
                Text("◀").foregroundColor(.white).frame(width: 26, height: 34)
            }
            .buttonStyle(.plain)
            Text(format(v)).font(.system(size: 13, design: .monospaced)).foregroundColor(Hw.lcdFg)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
            Button {
                e.setPartMix(k, id: id, value: min(hi, v + 1))
                tick += 1
            } label: {
                Text("▶").foregroundColor(.white).frame(width: 26, height: 34)
            }
            .buttonStyle(.plain)
        }
        .frame(width: width)
    }
}

// ---------------------------------------------------------------- system

/// Audio status and settings, recording, MIDI in / out.
struct SystemScreen: View {
    @EnvironmentObject var model: SynthModel
    @AppStorage("maxVoices") private var maxVoices = 128
    @AppStorage("quality") private var quality = 1
    @State private var tick = 0
    @State private var recSeconds = -1.0
    @State private var lastRecording = ""
    @State private var bleSheet: BluetoothMidiSheet.Mode?
    private let timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        let e = model.engine
        let _ = tick  // the status below is read again every half second
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Panel(title: "Audio") {
                    Lcd(text: e.audioDevice, size: 12)
                    Text(String(format: "VOICES %d (PEAK %d)   DSP %d%% (PEAK %d%%)   CALLBACK %d%%   LATE %d",
                                e.activeVoices, e.peakVoices, Int(e.cpuLoad * 100), Int(e.cpuLoadPeak * 100),
                                Int(e.callbackLoadMax * 100), e.lateCallbacks))
                        .font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.lcdFg)
                    HStack {
                        HwButton(title: "Reset Peaks") { e.resetPeaks() }
                        HwButton(title: "Audio Input", lit: e.audioInputOn) {
                            e.setAudioInput(!e.audioInputOn)
                            tick += 1
                        }
                    }
                    Text("Audio Input をオンにすると、マイクの音をボコーダーやルーパーに使えます（最初に許可を求めます）。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "Recording") {
                    HStack {
                        HwButton(title: recSeconds >= 0 ? "■ Stop" : "● Rec", lit: recSeconds >= 0) { toggleRecording() }
                        Lcd(text: recSeconds >= 0 ? String(format: "REC %.1f s", recSeconds)
                                                  : (lastRecording.isEmpty ? "—" : lastRecording), size: 13)
                    }
                    Text("出力（エフェクトのあと）を録音し、録りながら「ファイル」アプリの HSynth > recordings に WAV で保存します。長さの上限はありません。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                Panel(title: "Settings") {
                    HStack(spacing: 16) {
                        HStack(spacing: 4) {
                            Text("POLYPHONY").font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                            HwButton(title: "◀") { maxVoices = max(8, maxVoices - 8) }
                            Lcd(text: "\(maxVoices)", size: 14).frame(width: 56)
                            HwButton(title: "▶") { maxVoices = min(128, maxVoices + 8) }
                        }
                        Picker("Quality", selection: $quality) {
                            Text("Low").tag(0)
                            Text("Normal").tag(1)
                            Text("High").tag(2)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 240)
                    }
                    Text("同時発音数を減らすと、重い音色や曲でも音切れしにくくなります。Quality は Low ほど軽く、High ほどきれいです。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
                SetupPanel()
                Panel(title: "MIDI") {
                    Lcd(text: "IN: " + (e.midiInputs.isEmpty ? "(none)" : e.midiInputs.joined(separator: ", ")), size: 13)
                    HStack {
                        HwButton(title: "Refresh") {
                            e.refreshMidi()
                            tick += 1
                        }
                        Menu {
                            Button("Off") {
                                _ = e.openMidiOutput("")
                                tick += 1
                            }
                            ForEach(e.midiOutputs, id: \.self) { n in
                                Button(n) {
                                    _ = e.openMidiOutput(n)
                                    tick += 1
                                }
                            }
                        } label: {
                            Lcd(text: "OUT: " + (e.midiOutput.isEmpty ? "Off" : e.midiOutput), size: 13).frame(width: 280)
                        }
                    }
                    HStack {
                        HwButton(title: "Bluetooth MIDI") { bleSheet = .central }
                        HwButton(title: "Advertise") { bleSheet = .peripheral }
                    }
                    Text("MIDI OUT には、画面の鍵盤で弾いた音符を送ります。MIDI Learn は、Edit 画面の Learn をオンにしてつまみをタップし、コントローラーを動かすと割り当てられます。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                    Text("Bluetooth MIDI: BLE MIDI のキーボードやコントローラーを探して接続します。Advertise: この iPhone を Bluetooth MIDI 機器として公開し、Mac・PC・iPad から接続できるようにします（演奏も SysEx リモート操作も可。画面を開いている間だけ公開されます）。")
                        .font(.system(size: 11)).foregroundColor(Hw.engrave)
                }
            }
            .padding(8)
        }
        .sheet(isPresented: Binding(get: { bleSheet != nil }, set: { if !$0 { bleSheet = nil } })) {
            if let m = bleSheet {
                BluetoothMidiSheet(mode: m) {
                    bleSheet = nil
                    model.engine.refreshMidi()
                    tick += 1
                }
            }
        }
        .onReceive(timer) { _ in
            recSeconds = e.recordSeconds
            tick += 1
        }
        .onChange(of: maxVoices) { v in e.maxVoices = Int32(v) }
        .onChange(of: quality) { q in e.setQuality(Int32(q)) }
    }

    private func toggleRecording() {
        let e = model.engine
        if e.recordSeconds >= 0 {
            if let path = e.stopRecording() {
                lastRecording = (path as NSString).lastPathComponent
            } else {
                lastRecording = "(nothing recorded)"
            }
            recSeconds = -1
        } else {
            model.start()
            e.startRecording()
            recSeconds = 0
        }
    }
}

// ---------------------------------------------------------------- more editors

/// LFO 1 waveform (two cycles) and its rate.
struct LfoGraph: View {
    @EnvironmentObject var model: SynthModel

    var body: some View {
        let e = model.engine
        let waveId = e.paramId("lfo1.wave"), rateId = e.paramId("lfo1.rate")
        TimelineView(.periodic(from: .now, by: 0.2)) { _ in
            Canvas { c, size in
                guard waveId >= 0 else { return }
                let wave = Int(e.paramValue(waveId).rounded())
                let w = size.width, h = size.height
                let sh: [Double] = [0.6, -0.3, 0.9, -0.8, 0.2, -0.5, 0.75, -0.1]
                var p = Path()
                let n = 160
                for i in 0...n {
                    let ph = (Double(i) / Double(n) * 2).truncatingRemainder(dividingBy: 1)
                    var v: Double
                    switch wave {
                    case 1: v = ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4
                    case 2: v = 1 - ph * 2
                    case 3: v = ph < 0.5 ? 1 : -1
                    case 4: v = sh[min(7, Int(Double(i) / Double(n) * 8))]
                    default: v = sin(ph * 2 * Double.pi)
                    }
                    let pt = CGPoint(x: CGFloat(i) / CGFloat(n) * w, y: h / 2 - CGFloat(v) * (h / 2 - 6))
                    if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
                }
                c.stroke(p, with: .color(Hw.lcdFg), lineWidth: 2)
                if rateId >= 0 {
                    c.draw(Text(e.formatParam(rateId)).font(.system(size: 11, design: .monospaced)).foregroundColor(Hw.lcdDim),
                           at: CGPoint(x: w - 44, y: 10))
                }
            }
        }
        .frame(height: 70)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

/// Step sequencer: drag the bars for velocity (bottom = rest), the note row up / down for pitch.
struct SeqEditor: View {
    @EnvironmentObject var model: SynthModel
    @State private var tick = 0
    @State private var noteStep = -1
    @State private var noteStart: Float = 60
    @State private var velDragging = false

    private static let names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    private func pid(_ step: Int, _ field: String) -> Int32 { model.engine.paramId("seq.s\(step + 1).\(field)") }

    private func noteName(_ v: Float) -> String {
        let note = Int(v.rounded())
        return Self.names[((note % 12) + 12) % 12] + "\(note / 12 - 1)"
    }

    var body: some View {
        let e = model.engine
        let stepsId = e.paramId("seq.steps")
        let n = max(1, min(32, Int(stepsId >= 0 ? e.paramValue(stepsId) : 16)))
        let _ = tick
        VStack(spacing: 4) {
            GeometryReader { g in
                let w = g.size.width / CGFloat(n)
                HStack(spacing: 0) {
                    ForEach(0..<n, id: \.self) { s in
                        Text(noteName(e.paramValue(pid(s, "note"))))
                            .font(.system(size: n > 16 ? 8 : 10, design: .monospaced)).foregroundColor(Hw.lcdFg)
                            .frame(width: w, height: 24)
                            .background(Rectangle().fill(s % 4 == 0 ? Color(white: 0.13) : Hw.lcdBg))
                    }
                }
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0)
                    .onChanged { d in
                        if noteStep < 0 {
                            noteStep = min(n - 1, max(0, Int(d.startLocation.x / w)))
                            noteStart = e.paramValue(pid(noteStep, "note"))
                            model.checkpoint()
                        }
                        let v = min(127, max(0, (noteStart - Float(d.translation.height / 8)).rounded()))
                        e.setParam(pid(noteStep, "note"), value: v)
                        model.show("Step \(noteStep + 1)  " + noteName(v))
                        tick += 1
                    }
                    .onEnded { _ in noteStep = -1 })
            }
            .frame(height: 24)
            GeometryReader { g in
                let w = g.size.width / CGFloat(n), h = g.size.height
                TimelineView(.periodic(from: .now, by: 0.05)) { _ in
                    Canvas { c, _ in
                        let playing = Int(e.seqStep)
                        for s in 0..<n {
                            let vel = CGFloat(e.paramValue(pid(s, "vel")))
                            let r = CGRect(x: CGFloat(s) * w + 1, y: h - vel * h, width: max(1, w - 2), height: vel * h)
                            c.fill(Path(r), with: .color(s == playing ? Hw.led : Hw.lcdFg.opacity(0.85)))
                            if vel <= 0 {
                                c.fill(Path(CGRect(x: CGFloat(s) * w + 1, y: h - 2, width: max(1, w - 2), height: 2)),
                                       with: .color(s == playing ? Hw.led : Hw.lcdDim))
                            }
                        }
                    }
                }
                .contentShape(Rectangle())
                .gesture(DragGesture(minimumDistance: 0)
                    .onChanged { d in
                        if !velDragging {
                            velDragging = true
                            model.checkpoint()
                        }
                        let s = min(n - 1, max(0, Int(d.location.x / w)))
                        let v = Float(min(1, max(0, 1 - d.location.y / h)))
                        e.setParam(pid(s, "vel"), value: v < 0.05 ? 0 : v)
                        tick += 1
                    }
                    .onEnded { _ in velDragging = false })
            }
            .frame(height: 110)
            .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
            Text("棒を上下にドラッグすると強さ（いちばん下は休符）、上の音名を上下にドラッグすると音の高さが変わります。")
                .font(.system(size: 11)).foregroundColor(Hw.engrave).frame(maxWidth: .infinity, alignment: .leading)
        }
        .id(model.revision)
    }
}

/// MSEG: the points (time from the previous one, level); drag a point to move it.
struct MsegEditor: View {
    @EnvironmentObject var model: SynthModel
    @State private var tick = 0
    @State private var dragPoint = -1

    private func pid(_ p: Int, _ field: String) -> Int32 { model.engine.paramId("mseg.p\(p + 1).\(field)") }

    var body: some View {
        let e = model.engine
        let countId = e.paramId("mseg.points")
        let n = max(2, min(8, Int(countId >= 0 ? e.paramValue(countId) : 4)))
        let _ = tick
        var times: [Double] = []
        for i in 0..<n { times.append((times.last ?? 0) + Double(e.paramValue(pid(i, "time")))) }
        let total = max(0.5, (times.last ?? 1) * 1.15)
        return GeometryReader { g in
            let w = g.size.width, h = g.size.height
            let pts = (0..<n).map { i in
                CGPoint(x: CGFloat(times[i] / total) * w, y: (1 - CGFloat(e.paramValue(pid(i, "level")))) * (h - 12) + 6)
            }
            Canvas { c, _ in
                var p = Path()
                for (i, pt) in pts.enumerated() {
                    if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
                }
                c.stroke(p, with: .color(Hw.lcdFg), lineWidth: 2)
                for (i, pt) in pts.enumerated() {
                    c.fill(Path(ellipseIn: CGRect(x: pt.x - 6, y: pt.y - 6, width: 12, height: 12)),
                           with: .color(i == dragPoint ? Hw.led : Hw.lcdFg))
                }
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { d in
                    if dragPoint < 0 {
                        var best = 0
                        var bestD = Double.greatestFiniteMagnitude
                        for (i, pt) in pts.enumerated() {
                            let dd = hypot(Double(pt.x - d.startLocation.x), Double(pt.y - d.startLocation.y))
                            if dd < bestD {
                                bestD = dd
                                best = i
                            }
                        }
                        dragPoint = best
                        model.checkpoint()
                    }
                    let i = dragPoint
                    let level = Float(min(1, max(0, 1 - (d.location.y - 6) / (h - 12))))
                    e.setParam(pid(i, "level"), value: level)
                    let t = Double(min(w, max(0, d.location.x)) / w) * total
                    let prev = i > 0 ? times[i - 1] : 0
                    e.setParam(pid(i, "time"), value: Float(max(0, min(10, t - prev))))
                    model.show("P\(i + 1)  " + e.formatParam(pid(i, "time")) + "  " + e.formatParam(pid(i, "level")))
                    tick += 1
                }
                .onEnded { _ in dragPoint = -1 })
        }
        .frame(height: 120)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
        .id(model.revision)
    }
}

/// FM algorithm of a layer: operators (carriers at the bottom, lit) and who modulates whom.
struct FmAlgoView: View {
    @EnvironmentObject var model: SynthModel
    let layer: String  // "a", "b", "c"

    var body: some View {
        let e = model.engine
        let algoId = e.paramId(layer + ".fm.algo")
        TimelineView(.periodic(from: .now, by: 0.3)) { _ in
            Canvas { c, size in
                guard algoId >= 0 else { return }
                let a = e.fmAlgorithm(Int32(e.paramValue(algoId).rounded())).map { $0.intValue }
                guard a.count >= 11 else { return }
                let ops = max(1, min(8, a[0]))
                let carriers = a[1], fb = a[2]
                let mods = Array(a[3..<11])
                // depth: carriers 0, a modulator one more than the deepest operator it feeds
                var depth = [Int](repeating: -1, count: ops)
                for i in 0..<ops where carriers & (1 << i) != 0 { depth[i] = 0 }
                for _ in 0..<ops {
                    for t in 0..<ops where depth[t] >= 0 {
                        for m in 0..<ops where mods[t] & (1 << m) != 0 && m != t {
                            depth[m] = max(depth[m], depth[t] + 1)
                        }
                    }
                }
                let maxDepth = max(0, depth.max() ?? 0)
                var pos = [CGPoint](repeating: .zero, count: ops)
                for d in 0...maxDepth {
                    let row = (0..<ops).filter { depth[$0] == d }
                    for (k, i) in row.enumerated() {
                        let x = size.width * CGFloat(k + 1) / CGFloat(row.count + 1)
                        let y = size.height - 18 - CGFloat(d) * (size.height - 36) / CGFloat(max(1, maxDepth))
                        pos[i] = CGPoint(x: x, y: y)
                    }
                }
                for t in 0..<ops where depth[t] >= 0 {
                    for m in 0..<ops where mods[t] & (1 << m) != 0 && m != t && depth[m] >= 0 {
                        var l = Path()
                        l.move(to: pos[m])
                        l.addLine(to: pos[t])
                        c.stroke(l, with: .color(Hw.lcdDim), lineWidth: 1.5)
                    }
                }
                for i in 0..<ops where depth[i] >= 0 {
                    let r = CGRect(x: pos[i].x - 14, y: pos[i].y - 11, width: 28, height: 22)
                    let isCarrier = carriers & (1 << i) != 0
                    c.fill(Path(roundedRect: r, cornerRadius: 3), with: .color(isCarrier ? Hw.led : Color(white: 0.35)))
                    c.draw(Text("\(i + 1)").font(.system(size: 12, weight: .bold)).foregroundColor(.black), at: pos[i])
                    if i == fb {
                        c.stroke(Path(ellipseIn: CGRect(x: pos[i].x + 12, y: pos[i].y - 18, width: 12, height: 12)),
                                 with: .color(Hw.led), lineWidth: 1.5)
                    }
                }
            }
        }
        .frame(height: 130)
        .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
    }
}

/// The mod matrix slots in use: source (x via) -> destination, with the amount as a bar.
struct ModMatrixSummary: View {
    @EnvironmentObject var model: SynthModel

    private struct Row {
        let text: String
        let amount: Float
    }

    private func rows() -> [Row] {
        let e = model.engine
        var out: [Row] = []
        for s in 1...16 {
            let src = e.paramId("mod\(s).src"), via = e.paramId("mod\(s).via")
            let dst = e.paramId("mod\(s).dst"), amt = e.paramId("mod\(s).amt")
            guard src >= 0, dst >= 0, amt >= 0, e.paramValue(src) > 0.5, e.paramValue(dst) > 0.5 else { continue }
            let v = via >= 0 && e.paramValue(via) > 0.5 ? " x " + e.formatParam(via) : ""
            out.append(Row(text: "#\(s)  " + e.formatParam(src) + v + "  ->  " + e.formatParam(dst), amount: e.paramValue(amt)))
        }
        return out
    }

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.3)) { _ in
            let list = rows()
            VStack(alignment: .leading, spacing: 3) {
                if list.isEmpty {
                    Text("(no routings)").font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.lcdDim)
                }
                ForEach(list.indices, id: \.self) { i in
                    HStack(spacing: 8) {
                        Text(list[i].text).font(.system(size: 12, design: .monospaced)).foregroundColor(Hw.lcdFg).lineLimit(1)
                        Spacer()
                        AmountBar(value: list[i].amount).frame(width: 120, height: 8)
                    }
                }
            }
            .padding(8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
        }
    }
}

/// -1..1 as a bar from the middle.
struct AmountBar: View {
    let value: Float
    var body: some View {
        Canvas { c, size in
            c.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(white: 0.16)))
            let a = CGFloat(max(-1, min(1, value)))
            let mid = size.width / 2
            let r = a >= 0 ? CGRect(x: mid, y: 0, width: a * mid, height: size.height)
                           : CGRect(x: mid + a * mid, y: 0, width: -a * mid, height: size.height)
            c.fill(Path(r), with: .color(a >= 0 ? Hw.led : Color.cyan))
        }
    }
}

// ---------------------------------------------------------------- play: arp / tempo

/// Arpeggiator and tempo on the Play screen, with Run (the arp / sequencer clock).
struct ArpPanel: View {
    @EnvironmentObject var model: SynthModel
    @State private var running = false
    private let names = ["arp.on", "arp.mode", "arp.rate", "arp.octaves", "arp.latch", "fx.tempo"]

    var body: some View {
        let e = model.engine
        Panel(title: "Arp / Tempo") {
            HStack(alignment: .top, spacing: 6) {
                ForEach(names, id: \.self) { n in
                    let pid = e.paramId(n)
                    if pid >= 0 { ParamControl(param: pid) }
                }
                VStack(spacing: 4) {
                    HwButton(title: "Run", lit: running) {
                        e.transportRunning = !e.transportRunning
                        running = e.transportRunning
                    }
                    Text("CLOCK").font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
                }
            }
        }
        .onAppear { running = e.transportRunning }
    }
}

// ---------------------------------------------------------------- setup files

/// A setup file (engine setup.h) for the share / save sheet.
struct SetupDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.json] }
    var text: String
    init(text: String) { self.text = text }
    init(configuration: ReadConfiguration) throws {
        text = String(decoding: configuration.file.regularFileContents ?? Data(), as: UTF8.self)
    }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data(text.utf8))
    }
}

/// System screen: export / import the whole playing setup (parts, effects, MIDI learn, settings).
struct SetupPanel: View {
    @EnvironmentObject var model: SynthModel
    @State private var exporting = false
    @State private var importing = false
    @State private var document = SetupDocument(text: "")
    @State private var message = ""

    var body: some View {
        Panel(title: "Setup") {
            HStack {
                HwButton(title: "Export…") {
                    document = SetupDocument(text: model.setupText(name: "Setup"))
                    exporting = true
                }
                HwButton(title: "Import…") { importing = true }
            }
            if !message.isEmpty { Lcd(text: message, size: 12) }
            Text("パート（MIDI チャンネル・プリセット・ミックス）、エフェクト、MIDI Learn、同時発音数を 1 つのファイルに書き出し・読み込みします。PC 版・Android 版と共通の形式です。")
                .font(.system(size: 11)).foregroundColor(Hw.engrave)
        }
        .fileExporter(isPresented: $exporting, document: document, contentType: .json, defaultFilename: "setup") { result in
            if case .success = result { message = "書き出しました" }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.json, .plainText, .data], allowsMultipleSelection: false) { result in
            guard case .success(let urls) = result, let url = urls.first else { return }
            message = model.importSetup(url)
        }
    }
}

extension SynthModel {
    func setupText(name: String) -> String {
        let d = UserDefaults.standard
        let host: [String: Any] = ["voices": d.object(forKey: "maxVoices") as? Int ?? 128,
                                   "quality": d.object(forKey: "quality") as? Int ?? 1]
        let hostText = (try? JSONSerialization.data(withJSONObject: host)).map { String(decoding: $0, as: UTF8.self) } ?? "{}"
        return engine.setupJson(name, partNames: partNames, host: hostText)
    }

    /// Applies a picked setup file; returns a message for the user.
    func importSetup(_ url: URL) -> String {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else { return "読み込めませんでした" }
        if comparing { compare() }
        engine.songPlay(false)
        guard let r = engine.applySetup(String(decoding: data, as: UTF8.self)) else {
            return "読み込めませんでした: " + (engine.lastWarnings.first ?? "")
        }
        if (r["state"] as? Bool) == true {
            multi = (r["multi"] as? Bool) ?? false
            let parts = r["parts"] as? [String] ?? []
            for k in 0..<min(16, parts.count) { partNames[k] = parts[k] }
            editPart = Int(engine.editPart)
            let shown = partNames[editPart].isEmpty ? "Init" : partNames[editPart]
            presetName = multi ? "\(editPart + 1): \(shown)" : shown
            soundLoaded()
        }
        if let h = (r["host"] as? String)?.data(using: .utf8),
           let host = try? JSONSerialization.jsonObject(with: h) as? [String: Any] {
            let d = UserDefaults.standard
            if let v = host["voices"] as? Int {
                d.set(max(8, min(128, v)), forKey: "maxVoices")
                engine.maxVoices = Int32(max(8, min(128, v)))
            }
            if let q = host["quality"] as? Int {
                d.set(max(0, min(2, q)), forKey: "quality")
                engine.setQuality(Int32(max(0, min(2, q))))
            }
        }
        let name = r["name"] as? String ?? ""
        show("Setup \(name)")
        return "「\(name)」を読み込みました"
    }
}

// ---------------------------------------------------------------- multi: scenes

extension SynthModel {
    func scenePath(_ n: Int) -> String {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("scenes")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("scene\(n + 1).json").path
    }

    func storeScene(_ n: Int, name: String) -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        let nm = trimmed.isEmpty ? "Scene \(n + 1)" : trimmed
        let ok = engine.saveScene(scenePath(n), name: nm, partNames: partNames)
        if ok {
            var names = UserDefaults.standard.stringArray(forKey: "sceneNames") ?? []
            while names.count < 8 { names.append("") }
            names[n] = nm
            UserDefaults.standard.set(names, forKey: "sceneNames")
            show("Stored \(nm)")
        }
        return ok
    }

    func recallScene(_ n: Int) {
        guard FileManager.default.fileExists(atPath: scenePath(n)),
              let r = engine.loadScene(scenePath(n)), r.count >= 17 else {
            show("Scene \(n + 1) is empty")
            return
        }
        if comparing { compare() }
        engine.songPlay(false)
        multi = true
        for k in 0..<16 { partNames[k] = r[k + 1] }
        editPart = Int(engine.editPart)
        let shown = partNames[editPart].isEmpty ? "Init" : partNames[editPart]
        presetName = "\(editPart + 1): \(shown)"
        soundLoaded()
        show("Scene \(r[0])")
    }
}

/// The 8 scene buttons of the Multi screen (Store, then a button, saves the whole multi there).
struct SceneBar: View {
    @EnvironmentObject var model: SynthModel
    @State private var storeMode = false
    @State private var naming = false
    @State private var target = 0
    @State private var name = ""
    @State private var names = UserDefaults.standard.stringArray(forKey: "sceneNames") ?? []

    private func label(_ n: Int) -> String { n < names.count && !names[n].isEmpty ? names[n] : "\(n + 1)" }

    var body: some View {
        HStack(spacing: 6) {
            Text("SCENES").font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
            ForEach(0..<8, id: \.self) { n in
                HwButton(title: label(n)) {
                    if storeMode {
                        target = n
                        name = n < names.count ? names[n] : ""
                        naming = true
                    } else {
                        model.recallScene(n)
                    }
                }
            }
            HwButton(title: "Store", lit: storeMode) { storeMode.toggle() }
        }
        .alert("Store scene \(target + 1)", isPresented: $naming) {
            TextField("Name", text: $name)
            Button("Store") {
                _ = model.storeScene(target, name: name)
                names = UserDefaults.standard.stringArray(forKey: "sceneNames") ?? []
                storeMode = false
            }
            Button("Cancel", role: .cancel) {}
        }
    }
}
