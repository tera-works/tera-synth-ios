import AVFoundation
import SwiftUI

/// Shared engine state for the views.
final class SynthModel: ObservableObject {
    let engine: HSynthEngine
    private var midiPollTick = 0
    let player: PlayerModel
    let sets = SetListModel()
    @Published var presetName = "Init"
    @Published var presetIndex = -1
    @Published var voices = 0
    @Published var cpu: Float = 0
    @Published var revision = 0          // bumps when parameters change wholesale (preset load)
    @Published var lcdOverride: String? = nil
    // multi mode: the sound of every part, the part being edited
    @Published var multi = false
    @Published var editPart = 0
    @Published var partNames = [String](repeating: "", count: 16)
    // edit history of the edit part (undo / redo / compare with the loaded sound)
    @Published var canUndo = false
    @Published var canRedo = false
    @Published var comparing = false
    private var undoStack: [[NSNumber]] = []
    private var redoStack: [[NSNumber]] = []
    private var loadedSound: [NSNumber] = []
    private var editedSound: [NSNumber] = []
    // MIDI learn: tap a knob while learnMode is on, then move a controller
    @Published var learnMode = false
    @Published var learnTarget: Int32? = nil
    private var timer: Timer?
    private var lcdClear: DispatchWorkItem?
    private var audioOn = false
    private var background = false
    private var idleSince: Date?

    // SysEx remote control: a request changed the sound -> the views re-read the engine
    private var remoteSeen = 0
    private var remoteSongsSeen = 0
    func pollRemote() {
        let c = engine.remoteChanges
        if c != remoteSeen {
            remoteSeen = c
            revision += 1
        }
        let s = engine.remoteSongCount
        if s != remoteSongsSeen {
            remoteSongsSeen = s
            songLoaded()
        }
    }

    init() {
        let factory = Bundle.main.resourcePath ?? ""
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].path
        engine = HSynthEngine(factoryDir: factory, userDir: docs)
        player = PlayerModel(engine: engine)
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.voices = Int(self.engine.activeVoices)
            self.cpu = self.engine.cpuLoad
            self.checkBackgroundIdle()
            self.pollLearn()
            self.pollRemote()
            self.midiPollTick += 1
            if self.midiPollTick % 8 == 0 { _ = self.engine.pollMidi() }  // a MIDI device plugged in / out
        }
        engine.maxVoices = Int32(UserDefaults.standard.object(forKey: "maxVoices") as? Int ?? 128)
        engine.setQuality(Int32(UserDefaults.standard.object(forKey: "quality") as? Int ?? 1))
        player.ensureAudio = { [weak self] in self?.start() }
        player.onSongLoaded = { [weak self] in self?.songLoaded() }
        // a phone call or another app took the audio: pause the song, restart the audio afterwards
        NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
            guard let self, let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
            if type == .began {
                self.engine.songPlay(false)
            } else if !self.background || self.audioOn {
                self.engine.stopAudio()
                self.audioOn = self.engine.startAudio()
            }
        }
        let last = UserDefaults.standard.integer(forKey: "lastPreset")
        load(preset: engine.presetNames.indices.contains(last) ? last : 0)
    }

    private var autotestStarted = false

    func start() {
        if !audioOn { audioOn = engine.startAudio() }
        if (Self.autotestRequested || Self.libtestRequested) && !autotestStarted {
            autotestStarted = true
            if Self.libtestRequested { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.runLibtest() } }
            if Self.autotestRequested { runAutotest() }
        }
    }
    func stop() {
        engine.stopAudio()
        audioOn = false
    }

    /// Scene phase: a playing song keeps going in the background (UIBackgroundModes audio);
    /// otherwise the audio stops as before.
    func enterBackground() {
        background = true
        player.background = true
        idleSince = nil
        if !engine.songPlaying { stop() }
    }
    func enterForeground() {
        background = false
        player.background = false
        start()
    }

    /// Paused in the background for 10 minutes: stop the audio (iOS then suspends the app).
    private func checkBackgroundIdle() {
        guard background, audioOn else { return }
        if engine.songPlaying { idleSince = nil; return }
        if idleSince == nil { idleSince = Date() }
        else if Date().timeIntervalSince(idleSince!) > 600 { stop() }
    }

    /// Library: in multi mode the preset goes into the edit part, else it is the whole sound.
    func load(preset i: Int) {
        if engine.multiTimbral {
            guard engine.loadPreset(i, intoPart: Int32(editPart)) else { return }
            let names = engine.presetNames
            if names.indices.contains(i) { partNames[editPart] = names[i] }
            presetName = "\(editPart + 1): \(partNames[editPart])"
        } else {
            engine.songPlay(false)  // a preset replaces the song's sounds
            guard engine.loadPreset(i) else { return }
            presetIndex = i
            presetName = engine.currentPresetName
            UserDefaults.standard.set(i, forKey: "lastPreset")
            partNames[0] = presetName
            editPart = 0
            multi = false
        }
        soundLoaded()
    }

    /// A new sound in the edit part: the history starts again.
    func soundLoaded() {
        undoStack.removeAll()
        redoStack.removeAll()
        loadedSound = engine.allParams()
        comparing = false
        updateHistory()
        revision += 1
    }

    // ------------------------------------------------------------ parts

    func setMulti(_ on: Bool) {
        engine.multiTimbral = on
        multi = on
        if !on { selectPart(0) }
    }

    func selectPart(_ k: Int) {
        if comparing { compare() }
        engine.editPart = Int32(k)
        editPart = k
        presetName = multi ? "\(k + 1): \(partNames[k].isEmpty ? "Init" : partNames[k])" : presetName
        soundLoaded()
    }

    /// The Player loaded a song: every part has a GM sound now.
    func songLoaded() {
        multi = true
        for k in 0..<16 {
            let p = engine.songChannelPreset(Int32(k))
            partNames[k] = p.isEmpty ? partNames[k] : p
        }
        editPart = Int(engine.editPart)
        soundLoaded()
    }

    // ------------------------------------------------------------ history

    /// Before a change: the current sound goes on the undo stack.
    func checkpoint() {
        if comparing { compare() }
        undoStack.append(engine.allParams())
        if undoStack.count > 64 { undoStack.removeFirst() }
        redoStack.removeAll()
        updateHistory()
    }

    func undo() {
        guard let prev = undoStack.popLast() else { return }
        redoStack.append(engine.allParams())
        engine.setAllParams(prev)
        updateHistory()
        revision += 1
        show("Undo")
    }

    func redo() {
        guard let next = redoStack.popLast() else { return }
        undoStack.append(engine.allParams())
        engine.setAllParams(next)
        updateHistory()
        revision += 1
        show("Redo")
    }

    /// Toggles between the edited sound and the sound as loaded.
    func compare() {
        guard !loadedSound.isEmpty else { return }
        if comparing {
            engine.setAllParams(editedSound)
            comparing = false
        } else {
            editedSound = engine.allParams()
            engine.setAllParams(loadedSound)
            comparing = true
        }
        revision += 1
    }

    private func updateHistory() {
        canUndo = !undoStack.isEmpty
        canRedo = !redoStack.isEmpty
    }

    // ------------------------------------------------------------ MIDI learn

    func learn(_ param: Int32) {
        engine.learnArm(param)
        learnTarget = param
        show("Learn \(engine.paramLabel(param)): move a controller")
    }

    private func pollLearn() {
        guard let p = learnTarget, engine.learnArmed < 0 else { return }
        learnTarget = nil
        let d = engine.learnDescribe(p)
        if !d.isEmpty {
            engine.learnSave()
            show("\(engine.paramLabel(p)) <- \(d)")
            revision += 1
        }
    }

    func step(_ d: Int) {
        let n = engine.presetNames.count
        guard n > 0 else { return }
        load(preset: ((presetIndex < 0 ? 0 : presetIndex + d) % n + n) % n)
    }

    /// The header LCD shows "Label value" for a moment while a control moves.
    func show(_ text: String) {
        lcdOverride = text
        lcdClear?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.lcdOverride = nil }
        lcdClear = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: w)
    }
}

@main
struct HSynthApp: App {
    @StateObject private var model = SynthModel()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
                .statusBarHidden(true)
                .persistentSystemOverlays(.hidden)
        }
        .onChange(of: phase) { p in
            if p == .active { model.enterForeground() } else if p == .background { model.enterBackground() }
        }
    }
}
