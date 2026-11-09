import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: SynthModel
    @State private var tab = 0

    var body: some View {
        VStack(spacing: 0) {
            header
            Group {
                switch tab {
                case 1: EditScreen()
                case 2: MultiScreen()
                case 3: LiveScreen(sets: model.sets)
                case 4: PlayerScreen(player: model.player)
                case 5: SampleScreen()
                case 6: LibraryScreen(tab: $tab)
                case 7: SystemScreen()
                default: PlayScreen()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Hw.chassis.ignoresSafeArea())
    }

    private var header: some View {
        HStack(spacing: 10) {
            (Text("H").foregroundColor(Hw.led) + Text("SYNTH").foregroundColor(.white))
                .font(.system(size: 18, weight: .black)).tracking(2)
            VStack(alignment: .leading, spacing: 1) {
                Text(model.lcdOverride ?? model.presetName)
                    .font(.system(size: 16, design: .monospaced)).foregroundColor(Hw.lcdFg).lineLimit(1)
                Text(String(format: "VOICE %d  DSP %d%%", model.voices, Int(model.cpu * 100)))
                    .font(.system(size: 10, design: .monospaced)).foregroundColor(Hw.lcdDim)
            }
            .padding(.horizontal, 8).padding(.vertical, 3)
            .frame(maxWidth: 220, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
            Spacer(minLength: 4)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(Array(["Play", "Edit", "Multi", "Live", "Player", "Sample", "Library", "System"].enumerated()), id: \.offset) { i, t in
                        HwButton(title: t, lit: tab == i) { tab = i }
                    }
                }
            }
            .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(LinearGradient(colors: [Color(white: 0.16), Color(white: 0.09)], startPoint: .top, endPoint: .bottom))
    }
}

// ---------------------------------------------------------------- play

struct PlayScreen: View {
    @EnvironmentObject var model: SynthModel
    @AppStorage("firstNote") private var firstNote = 48
    @AppStorage("whiteKeys") private var whiteKeys = 15
    @State private var hold = false

    private let quick = ["master.volume", "filter.cutoff", "filter.reso", "amp.attack", "amp.release"]

    var body: some View {
        let e = model.engine
        VStack(spacing: 8) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(alignment: .top, spacing: 8) {
                    Panel(title: "Preset") {
                        Lcd(text: model.presetName, size: 20).frame(width: 190)
                        HStack {
                            HwButton(title: "◀") { model.step(-1) }
                            HwButton(title: "▶") { model.step(1) }
                            HwButton(title: "All Off") { e.allSoundOff() }
                        }
                    }
                    Panel(title: "Sound") {
                        HStack(spacing: 2) {
                            ForEach(quick, id: \.self) { n in
                                let id = e.paramId(n)
                                if id >= 0 { Knob(param: id) }
                            }
                        }
                    }
                    ArpPanel()
                    PerformPanel()
                    PhrasePanel()
                    Panel(title: "Keyboard") {
                        HStack {
                            stepper("Octave", value: "\((firstNote - 48) / 12)") {
                                firstNote = max(12, firstNote - 12)
                            } up: { firstNote = min(96, firstNote + 12) }
                            stepper("Keys", value: "\(whiteKeys)") {
                                whiteKeys = max(5, whiteKeys - 7)
                            } up: { whiteKeys = min(36, whiteKeys + 7) }
                        }
                        HwButton(title: "Hold", lit: hold) { hold.toggle(); e.sustain(hold) }
                    }
                }
                .padding(.horizontal, 8)
            }
            HStack(spacing: 6) {
                Wheel(title: "BEND", spring: true) { e.pitchBend($0) }.frame(width: 50)
                Wheel(title: "MOD", spring: false) { e.modWheel($0) }.frame(width: 50)
                KeyboardView(engine: e, firstNote: $firstNote, whiteKeys: $whiteKeys)
            }
            .padding(.top, 18)
            .padding([.horizontal, .bottom], 6)
        }
        .padding(.top, 8)
    }

    private func stepper(_ title: String, value: String, down: @escaping () -> Void, up: @escaping () -> Void) -> some View {
        VStack(spacing: 3) {
            HStack(spacing: 3) {
                HwButton(title: "◀", action: down)
                Lcd(text: value, size: 14).frame(width: 38)
                HwButton(title: "▶", action: up)
            }
            Text(title.uppercased()).font(.system(size: 10.5, weight: .bold)).foregroundColor(Hw.engrave)
        }
    }
}

// ---------------------------------------------------------------- edit

/// Every engine parameter, grouped like the engine's metadata (pages by function).
struct EditScreen: View {
    @EnvironmentObject var model: SynthModel
    @State private var page = 0

    private let pages: [(String, [String])] = [
        ("OSC", ["Layer A", "Layer A / VA", "Layer A / FM", "Layer A / PCM", "Layer A / WT", "Layer A / Organ",
                 "Layer A / Granular", "Layer B", "Layer B / VA", "Layer B / Granular", "Layer C", "Layer C / VA"]),
        ("Filter", ["Filter", "Filter 2", "Voice FX"]),
        ("Env", ["Amp Env", "Mod Env", "Env 3"]),
        ("LFO", ["LFO"]),
        ("Mod", ["Mod Matrix", "Macros", "Controllers"]),
        ("Arp / Seq", ["Arpeggiator", "Sequencer", "Performance"]),
        ("Motion", ["MSEG", "Motion 1", "Motion 2", "Motion 3", "Motion 4"]),
        ("FX", ["FX Chain", "Send FX", "Sends", "FX Mod"]),
        ("Voice", ["Voice", "Tuning", "Master"]),
    ]

    var body: some View {
        let e = model.engine
        let all = (0..<Int32(e.paramCount())).map { ($0, e.paramGroup($0)) }
        VStack(spacing: 6) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack {
                    if model.multi {
                        Lcd(text: "PART \(model.editPart + 1)", size: 14).frame(width: 90)
                    }
                    HwButton(title: "Undo") { model.undo() }.opacity(model.canUndo ? 1 : 0.4)
                    HwButton(title: "Redo") { model.redo() }.opacity(model.canRedo ? 1 : 0.4)
                    HwButton(title: "Compare", lit: model.comparing) { model.compare() }
                    HwButton(title: "Learn", lit: model.learnMode) {
                        model.learnMode.toggle()
                        if !model.learnMode {
                            e.learnCancel()
                            model.learnTarget = nil
                        }
                    }
                    Rectangle().fill(Color(white: 0.3)).frame(width: 1, height: 30)
                    ForEach(pages.indices, id: \.self) { i in HwButton(title: pages[i].0, lit: page == i) { page = i } }
                }
                .padding(.horizontal, 8)
            }
            ScrollView {
                VStack(spacing: 8) {
                    if page == 0 { SamplesPanel() }
                    ForEach(pages[page].1, id: \.self) { g in
                        let ids = all.filter { $0.1 == g }.map { $0.0 }
                        if !ids.isEmpty {
                            Panel(title: g) {
                                if g == "Amp Env" { EnvGraph(prefix: "amp") }
                                if g == "Mod Env" { EnvGraph(prefix: "env2") }
                                if g == "Filter" { FilterGraph() }
                                if g == "LFO" { LfoGraph() }
                                if g == "Sequencer" { SeqEditor() }
                                if g == "MSEG" { MsegEditor() }
                                if g == "Layer A / FM" { FmAlgoView(layer: "a") }
                                if g == "Mod Matrix" { ModMatrixSummary() }
                                LazyVGrid(columns: [GridItem(.adaptive(minimum: 70), spacing: 8)], alignment: .leading, spacing: 8) {
                                    ForEach(ids, id: \.self) { ParamControl(param: $0) }
                                }
                            }
                        }
                    }
                }
                .padding(8)
                .id(model.revision)
            }
        }
        .padding(.top, 8)
    }
}

// ---------------------------------------------------------------- library

struct LibraryScreen: View {
    @EnvironmentObject var model: SynthModel
    @Binding var tab: Int
    @State private var search = ""
    @State private var category = ""
    @State private var saving = false
    @State private var newName = ""

    var body: some View {
        let e = model.engine
        let names = e.presetNames
        let cats = e.presetCategories
        let shown = names.indices.filter { i in
            (category.isEmpty || cats[i] == category) &&
            (search.isEmpty || (names[i] + " " + cats[i]).localizedCaseInsensitiveContains(search))
        }
        VStack(spacing: 8) {
            HStack {
                TextField("Search", text: $search)
                    .textFieldStyle(.plain).font(.system(size: 15, design: .monospaced)).foregroundColor(Hw.lcdFg)
                    .padding(8).background(RoundedRectangle(cornerRadius: 2).fill(Hw.lcdBg))
                HwButton(title: "Save as…") { newName = model.presetName; saving = true }
            }
            .padding(.horizontal, 8)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack {
                    HwButton(title: "All \(names.count)", lit: category.isEmpty) { category = "" }
                    ForEach(Array(Set(cats)).sorted(), id: \.self) { c in
                        HwButton(title: "\(c) \(cats.filter { $0 == c }.count)", lit: category == c) { category = c }
                    }
                }
                .padding(.horizontal, 8)
            }
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 170), spacing: 8)], spacing: 8) {
                    ForEach(shown, id: \.self) { i in
                        Button {
                            model.load(preset: i)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(names[i]).font(.system(size: 14, weight: .semibold)).lineLimit(1)
                                Text(cats[i]).font(.system(size: 11)).opacity(0.7)
                            }
                            .foregroundColor(i == model.presetIndex ? Hw.lcdFg : .white)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(10)
                            .background(RoundedRectangle(cornerRadius: 3).fill(i == model.presetIndex ? Hw.lcdBg : Hw.key))
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(8)
            }
        }
        .padding(.top, 8)
        .alert("Save preset", isPresented: $saving) {
            TextField("Name", text: $newName)
            Button("Save") { if e.savePresetNamed(newName) { model.revision += 1 } }
            Button("Cancel", role: .cancel) {}
        }
    }
}
