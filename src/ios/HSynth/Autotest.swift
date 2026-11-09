import UIKit

/// `--autotest [--mute]` launch argument: plays every preset (a wide chord,
/// then a fast run) and writes the audio timing to Documents/autotest/report.txt
/// (read it with `xcrun devicectl device copy from --domain-type appDataContainer`).
/// Same idea as the desktop `--autotest`, for checking dropouts on a device.
extension SynthModel {
    static var autotestRequested: Bool { ProcessInfo.processInfo.arguments.contains("--autotest") }

    static var libtestRequested: Bool { ProcessInfo.processInfo.arguments.contains("--libtest") }

    /// `--libtest`: loads every preset and every song in Documents/midi without playing, and writes
    /// the load warnings (missing samples, ...) and each song's sounds per channel to
    /// Documents/autotest/library.txt.
    func runLibtest() {
        var lines = ["HSynth iOS library test \(Date())", "library: \(engine.libraryDir)"]
        let names = engine.presetNames, cats = engine.presetCategories
        var bad = 0
        for i in names.indices {
            let ok = engine.loadPreset(i)
            let w = engine.lastWarnings
            if !ok || !w.isEmpty { bad += 1 }
            lines.append("preset [\(cats[i])] \(names[i]): \(ok ? "ok" : "FAILED")\(w.isEmpty ? "" : " " + w.joined(separator: "; "))")
        }
        lines.append("presets \(names.count), with problems \(bad)")
        let songs = ((try? FileManager.default.contentsOfDirectory(atPath: engine.midiDir)) ?? [])
            .filter { $0.lowercased().hasSuffix(".mid") || $0.lowercased().hasSuffix(".midi") }.sorted()
        for f in songs {
            let err = engine.loadSong((engine.midiDir as NSString).appendingPathComponent(f))
            lines.append("song \(f): \(err ?? "ok")")
            for r in engine.lastWarnings { lines.append("  report: \(r)") }
            for ch in 0..<16 where engine.songChannelUsed(Int32(ch)) {
                lines.append("  ch\(ch + 1) \(engine.songChannelGm(Int32(ch))) -> \(engine.songChannelPreset(Int32(ch)))")
            }
        }
        engine.songPlay(false)
        lines.append("done")
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("autotest")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? lines.joined(separator: "\n").appending("\n")
            .write(to: dir.appendingPathComponent("library.txt"), atomically: true, encoding: .utf8)
        let last = UserDefaults.standard.integer(forKey: "lastPreset")
        load(preset: names.indices.contains(last) ? last : 0)
    }

    func runAutotest() {
        let mute = ProcessInfo.processInfo.arguments.contains("--mute")
        UIApplication.shared.isIdleTimerDisabled = true
        engine.muted = mute
        Task { @MainActor in
            var lines = ["HSynth iOS autotest \(Date())",
                         "device: \(UIDevice.current.model) iOS \(UIDevice.current.systemVersion)",
                         "muted: \(mute)"]
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            lines.append("audio: \(engine.audioDevice)")
            lines.append("preset | voices | engine load peak | callback load max | callback us avg | late callbacks")
            var worst: Float = 0
            var lateTotal = 0
            let names = engine.presetNames
            for i in names.indices {
                load(preset: i)
                try? await Task.sleep(nanoseconds: 400_000_000)
                engine.resetPeaks()
                // wide chord, held
                let chord = [36, 48, 55, 60, 64, 67, 72, 76, 79, 84]
                for k in chord { engine.noteOn(Int32(k), velocity: 0.8) }
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                for k in chord { engine.noteOff(Int32(k)) }
                // fast run (16ths at ~150 bpm, overlapping releases)
                for s in 0..<24 {
                    let k = 48 + (s * 5) % 36
                    engine.noteOn(Int32(k), velocity: 0.7)
                    try? await Task.sleep(nanoseconds: 50_000_000)
                    engine.noteOff(Int32(k))
                }
                try? await Task.sleep(nanoseconds: 600_000_000)
                let late = Int(engine.lateCallbacks)
                worst = max(worst, engine.callbackLoadMax)
                lateTotal += late
                lines.append(String(format: "%@ | %d | %.2f | %.2f | %.0f | %d", names[i], engine.peakVoices,
                                    engine.cpuLoadPeak, engine.callbackLoadMax, engine.callbackUsAvg, late))
                engine.allSoundOff()
            }
            lines.append(String(format: "worst callback load %.2f, late callbacks %d", worst, lateTotal))
            lines.append("done")
            let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("autotest")
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try? lines.joined(separator: "\n").appending("\n")
                .write(to: dir.appendingPathComponent("report.txt"), atomically: true, encoding: .utf8)
            engine.muted = false
            UIApplication.shared.isIdleTimerDisabled = false
        }
    }
}
