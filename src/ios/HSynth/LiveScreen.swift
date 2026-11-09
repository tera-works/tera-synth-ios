import SwiftUI

// Set lists for playing live: songs' sounds in running order, recalled with one tap
// (or Prev / Next). An entry is a Library preset (by name), a snapshot of the sound
// as it was edited, or a snapshot of a whole multi; snapshots are files of their own
// in Documents/setlists, so later edits or scene overwrites do not change them.

struct SetEntry: Codable, Identifiable, Equatable {
    enum Kind: String, Codable { case library, sound, scene }
    var id = UUID()
    var kind: Kind
    var name: String
    var file = ""  // snapshot file in Documents/setlists (sound / scene)
}

struct SetList: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var entries: [SetEntry]
}

final class SetListModel: ObservableObject {
    @Published var lists: [SetList] = []
    @Published var active: UUID? = nil
    @Published var current = -1  // entry recalled last
    let dir: URL

    init() {
        dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("setlists")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        load()
    }

    var activeList: SetList? { lists.first { $0.id == active } }
    var entries: [SetEntry] { activeList?.entries ?? [] }

    func path(_ file: String) -> String { dir.appendingPathComponent(file).path }

    private struct FileData: Codable {
        var active: UUID?
        var lists: [SetList]
    }

    private var indexURL: URL { dir.appendingPathComponent("setlists.json") }

    private func load() {
        guard let data = try? Data(contentsOf: indexURL), let f = try? JSONDecoder().decode(FileData.self, from: data) else { return }
        lists = f.lists
        active = f.lists.contains { $0.id == f.active } ? f.active : f.lists.first?.id
    }

    func save() {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted]
        if let data = try? enc.encode(FileData(active: active, lists: lists)) { try? data.write(to: indexURL, options: .atomic) }
    }

    func select(_ id: UUID) {
        active = id
        current = -1
        save()
    }

    func newList(_ name: String) {
        let n = name.trimmingCharacters(in: .whitespaces)
        let list = SetList(name: n.isEmpty ? "Set \(lists.count + 1)" : n, entries: [])
        lists.append(list)
        select(list.id)
    }

    func renameList(_ name: String) {
        guard let i = lists.firstIndex(where: { $0.id == active }) else { return }
        let n = name.trimmingCharacters(in: .whitespaces)
        if !n.isEmpty { lists[i].name = n }
        save()
    }

    func deleteList() {
        guard let i = lists.firstIndex(where: { $0.id == active }) else { return }
        for e in lists[i].entries where !e.file.isEmpty { try? FileManager.default.removeItem(atPath: path(e.file)) }
        lists.remove(at: i)
        active = lists.first?.id
        current = -1
        save()
    }

    func add(_ e: SetEntry) {
        guard let i = lists.firstIndex(where: { $0.id == active }) else { return }
        lists[i].entries.append(e)
        save()
    }

    func move(at k: Int, by d: Int) {
        guard let i = lists.firstIndex(where: { $0.id == active }),
              lists[i].entries.indices.contains(k), lists[i].entries.indices.contains(k + d) else { return }
        lists[i].entries.swapAt(k, k + d)
        if current == k { current = k + d } else if current == k + d { current = k }
        save()
    }

    func remove(at k: Int) {
        guard let i = lists.firstIndex(where: { $0.id == active }), lists[i].entries.indices.contains(k) else { return }
        let e = lists[i].entries.remove(at: k)
        if !e.file.isEmpty { try? FileManager.default.removeItem(atPath: path(e.file)) }
        if current == k { current = -1 } else if current > k { current -= 1 }
        save()
    }

    func rename(at k: Int, _ name: String) {
        guard let i = lists.firstIndex(where: { $0.id == active }), lists[i].entries.indices.contains(k) else { return }
        let n = name.trimmingCharacters(in: .whitespaces)
        if !n.isEmpty { lists[i].entries[k].name = n }
        save()
    }
}

extension SynthModel {
    /// Adds the sound as it is now (the single sound, or the whole multi) to the active set list.
    func addCurrentSound(to sets: SetListModel) {
        guard sets.activeList != nil else { return }
        if multi {
            let file = UUID().uuidString + ".scene.json"
            let first = partNames.first { !$0.isEmpty } ?? "Multi"
            guard engine.saveScene(sets.path(file), name: first, partNames: partNames) else {
                show("Cannot save the multi")
                return
            }
            sets.add(SetEntry(kind: .scene, name: "Multi: " + first, file: file))
        } else {
            let file = UUID().uuidString + ".sound.json"
            guard engine.saveSoundFile(sets.path(file), name: presetName) else {
                show("Cannot save the sound")
                return
            }
            sets.add(SetEntry(kind: .sound, name: presetName, file: file))
        }
        show("Added to \(sets.activeList?.name ?? "")")
    }

    /// Copies a scene slot (Multi screen) into the set list.
    func addSceneSlot(_ n: Int, to sets: SetListModel) {
        guard sets.activeList != nil, FileManager.default.fileExists(atPath: scenePath(n)) else {
            show("Scene \(n + 1) is empty")
            return
        }
        let file = UUID().uuidString + ".scene.json"
        do {
            try FileManager.default.copyItem(atPath: scenePath(n), toPath: sets.path(file))
        } catch {
            show("Cannot copy scene \(n + 1)")
            return
        }
        let names = UserDefaults.standard.stringArray(forKey: "sceneNames") ?? []
        let label = n < names.count && !names[n].isEmpty ? names[n] : "Scene \(n + 1)"
        sets.add(SetEntry(kind: .scene, name: label, file: file))
    }

    func recall(_ k: Int, in sets: SetListModel) {
        let list = sets.entries
        guard list.indices.contains(k) else { return }
        let e = list[k]
        switch e.kind {
        case .library:
            guard let i = engine.presetNames.firstIndex(of: e.name) else {
                show("\(e.name): not in the library")
                return
            }
            if multi { setMulti(false) }
            load(preset: i)
        case .sound:
            if comparing { compare() }
            engine.songPlay(false)
            guard let name = engine.loadSoundFile(sets.path(e.file)) else {
                show("\(e.name): cannot load")
                return
            }
            presetIndex = -1
            presetName = name
            multi = false
            editPart = 0
            partNames[0] = name
            soundLoaded()
        case .scene:
            recallSceneFile(sets.path(e.file), label: e.name)
        }
        sets.current = k
    }

    /// A whole multi from a scene file (scene slots, set lists).
    func recallSceneFile(_ path: String, label: String) {
        guard FileManager.default.fileExists(atPath: path), let r = engine.loadScene(path), r.count >= 17 else {
            show("\(label): cannot load")
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
        show(label)
    }
}

/// Live: the active set list as big buttons, Prev / Next, and the keyboard to play.
struct LiveScreen: View {
    @EnvironmentObject var model: SynthModel
    @ObservedObject var sets: SetListModel
    @AppStorage("firstNote") private var firstNote = 48
    @AppStorage("whiteKeys") private var whiteKeys = 15
    @State private var editing = false
    @State private var picking = false
    @State private var naming = false
    @State private var renaming = false
    @State private var renamingEntry = -1
    @State private var deleting = false
    @State private var text = ""

    private func step(_ d: Int) {
        let n = sets.entries.count
        guard n > 0 else { return }
        let k = sets.current < 0 ? (d > 0 ? 0 : n - 1) : min(n - 1, max(0, sets.current + d))
        model.recall(k, in: sets)
    }

    var body: some View {
        let e = model.engine
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Menu {
                    ForEach(sets.lists) { l in
                        Button(l.name) { sets.select(l.id) }
                    }
                    Divider()
                    Button("New Set List…") {
                        text = ""
                        naming = true
                    }
                } label: {
                    Lcd(text: (sets.activeList?.name ?? "No set list") + " ▾", size: 15).frame(width: 200)
                }
                Lcd(text: sets.entries.indices.contains(sets.current)
                    ? "\(sets.current + 1)/\(sets.entries.count)  \(sets.entries[sets.current].name)" : "—", size: 18)
                HwButton(title: "◀ Prev") { step(-1) }
                HwButton(title: "Next ▶") { step(1) }
                HwButton(title: "Edit", lit: editing) { editing.toggle() }
            }
            .padding(.horizontal, 8)

            if editing {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        HwButton(title: "+ Current Sound") { model.addCurrentSound(to: sets) }
                        HwButton(title: "+ Library…") { picking = true }
                        Menu {
                            ForEach(0..<8, id: \.self) { n in
                                Button("Scene \(n + 1)") { model.addSceneSlot(n, to: sets) }
                            }
                        } label: {
                            Text("+ Scene ▾").font(.system(size: 14, weight: .bold)).foregroundColor(.white)
                                .padding(.horizontal, 12).padding(.vertical, 9).frame(minHeight: 40)
                                .background(RoundedRectangle(cornerRadius: 3).fill(Hw.key))
                        }
                        HwButton(title: "Rename List") {
                            text = sets.activeList?.name ?? ""
                            renaming = true
                        }
                        HwButton(title: "Delete List") { deleting = true }
                    }
                    .padding(.horizontal, 8)
                }
                .disabled(sets.activeList == nil)
                .opacity(sets.activeList == nil ? 0.4 : 1)
            }

            ScrollView {
                if sets.activeList == nil {
                    Text("セットリストがありません。上の ▾ から「New Set List…」で作り、Edit で音色やシーンを追加してください。")
                        .font(.system(size: 13)).foregroundColor(Hw.engrave).padding()
                } else if sets.entries.isEmpty {
                    Text("まだ項目がありません。Edit をオンにして、「+ Current Sound」（今の音をそのまま保存）、「+ Library…」、「+ Scene」で追加してください。")
                        .font(.system(size: 13)).foregroundColor(Hw.engrave).padding()
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 190), spacing: 8)], spacing: 8) {
                    ForEach(Array(sets.entries.enumerated()), id: \.element.id) { k, entry in
                        entryButton(k, entry)
                    }
                }
                .padding(.horizontal, 8)
            }

            HStack(spacing: 6) {
                Wheel(title: "BEND", spring: true) { e.pitchBend($0) }.frame(width: 44)
                Wheel(title: "MOD", spring: false) { e.modWheel($0) }.frame(width: 44)
                KeyboardView(engine: e, firstNote: $firstNote, whiteKeys: $whiteKeys)
            }
            .frame(height: 150)
            .padding(.top, 14)
            .padding([.horizontal, .bottom], 6)
        }
        .padding(.top, 8)
        .sheet(isPresented: $picking) {
            PresetPicker(title: "Add to \(sets.activeList?.name ?? "")") { i in
                let names = model.engine.presetNames
                if names.indices.contains(i) { sets.add(SetEntry(kind: .library, name: names[i])) }
            }
            .environmentObject(model)
        }
        .alert("New set list", isPresented: $naming) {
            TextField("Name", text: $text)
            Button("Create") { sets.newList(text) }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Rename set list", isPresented: $renaming) {
            TextField("Name", text: $text)
            Button("Rename") { sets.renameList(text) }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Rename entry", isPresented: Binding(get: { renamingEntry >= 0 }, set: { if !$0 { renamingEntry = -1 } })) {
            TextField("Name", text: $text)
            Button("Rename") {
                sets.rename(at: renamingEntry, text)
                renamingEntry = -1
            }
            Button("Cancel", role: .cancel) { renamingEntry = -1 }
        }
        .alert("Delete this set list?", isPresented: $deleting) {
            Button("Delete", role: .destructive) {
                sets.deleteList()
                editing = false
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("保存した音色の控えも一緒に消えます。Library のプリセットとシーンは消えません。")
        }
    }

    private func kindLabel(_ k: SetEntry.Kind) -> String {
        switch k {
        case .library: return "PRESET"
        case .sound: return "SOUND"
        case .scene: return "MULTI"
        }
    }

    private func entryButton(_ k: Int, _ entry: SetEntry) -> some View {
        let on = k == sets.current
        return VStack(spacing: 4) {
            Button { model.recall(k, in: sets) } label: {
                HStack(spacing: 8) {
                    Text("\(k + 1)").font(.system(size: 22, weight: .black, design: .monospaced))
                        .foregroundColor(on ? .black : Hw.led).frame(width: 40)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(entry.name).font(.system(size: 15, weight: .bold)).foregroundColor(on ? .black : .white).lineLimit(1)
                        Text(kindLabel(entry.kind)).font(.system(size: 10, weight: .bold)).foregroundColor(on ? .black : Hw.engrave)
                    }
                    Spacer(minLength: 0)
                }
                .padding(10)
                .frame(maxWidth: .infinity, minHeight: 60)
                .background(RoundedRectangle(cornerRadius: 4).fill(on ? Hw.led : Hw.key))
            }
            .buttonStyle(.plain)
            if editing {
                HStack(spacing: 4) {
                    HwButton(title: "◀") { sets.move(at: k, by: -1) }
                    HwButton(title: "▶") { sets.move(at: k, by: 1) }
                    HwButton(title: "Aa") {
                        text = entry.name
                        renamingEntry = k
                    }
                    HwButton(title: "✕") { sets.remove(at: k) }
                }
            }
        }
    }
}
