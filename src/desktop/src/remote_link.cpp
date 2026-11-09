#include "remote_link.h"

RemoteLink::RemoteLink(hsynth::Engine& engine, MidiIn& in, hsynth::RemoteControl::Config config,
                       hsynth::RemoteControl::Hooks hooks)
    : in_(in) {
    control_ = std::make_unique<hsynth::RemoteControl>(engine, std::move(config), std::move(hooks));
    control_->setOutput([this](const std::vector<uint8_t>& m) { send(m); });
    in_.setSysExHandler([this](const uint8_t* data, size_t len, const std::string& replyPort) {
        hsynth::sysex::Frame f;
        if (!hsynth::sysex::parseFrame(data, len, f) || f.type != hsynth::sysex::Request) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!replyPort.empty()) wanted_ = replyPort;  // the port is (re)opened on the sending thread
        }
        requests_.fetch_add(1);
        control_->onSysEx(data, len);
    });
}

RemoteLink::~RemoteLink() {
    in_.setSysExHandler(nullptr);
    control_.reset();  // joins the worker before the output goes
    out_.close();
}

std::string RemoteLink::replyPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wanted_;
}

void RemoteLink::send(const std::vector<uint8_t>& msg) {
    std::string port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        port = wanted_;
    }
    if (port.empty()) return;  // nobody asked yet
    if (port == MidiIn::kVirtualPort) {
        in_.sendVirtual(msg.data(), msg.size());
        return;
    }
    if (port != opened_ || !out_.isOpen()) {
        std::string err;
        opened_ = port;
        if (!out_.open(port, err)) return;
    }
    out_.sendSysEx(msg.data(), msg.size());
}
