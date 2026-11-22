#include <M5Unified.h>
#include <atomic>
#include <cmath>
#include <esp_heap_caps.h>
#include "Portraits.h"

namespace {

constexpr uint32_t kMicRate = 16000;
constexpr uint32_t kVoiceRate = 24000;
constexpr size_t kMicFrame = 512;
constexpr size_t kMicFrames = kMicRate * 12 / kMicFrame;
constexpr size_t kVoiceBytes = kVoiceRate * 2 * 12;
constexpr unsigned kWidth = 240;
constexpr unsigned kHeight = 135;
constexpr uint8_t kVoiceVolume = 220;
constexpr uint32_t kCharacterMs = 70;
constexpr uint32_t kPageHoldMs = 1400;

enum class Phase { Idle, Recording, Finishing, Sending, Thinking, Receiving, Playing };
enum class Mood { Idle, Happy, Annoyed, Sleepy, Dizzy, Surprised, Sad };

M5Canvas scene(&M5.Display);
Phase phase = Phase::Idle;
Mood mood = Mood::Idle;
int16_t* mic = nullptr;
uint8_t* voice = nullptr;
std::atomic<size_t> micFilled{0};
size_t micQueued = 0, micSent = 0, voiceSize = 0, voiceReceived = 0;
unsigned turn = 0;
String caption;
String audioId;
String pendingCaption;
Mood pendingMood = Mood::Idle;
String rx;
bool discardLine = false;
bool hostSeen = false, muted = false, night = false;
bool petHeld = false, petLong = false;
bool inverted = false, hasImu = false;
uint32_t hostAt = 0, petAt = 0, phaseAt = 0, lastAction = 0;
uint32_t captionAt = 0, captionDuration = 0;
uint32_t voiceDrainedAt = 0;
bool voiceDraining = false;
uint32_t nextFrame = 0, lastImu = 0, gestureAt = 0;
uint32_t tiltStarted = 0;
float shakeEnergy = 0;
int uprightAxis = -1;
float uprightSign = 1;

struct CaptionPage {
    String lines[2];
    unsigned glyphs[2] = {};
    unsigned end = 0;
    uint32_t started = 0;
    bool ready = false;
};

CaptionPage captionPage;

bool elapsed(uint32_t now, uint32_t start, uint32_t duration) {
    return uint32_t(now - start) >= duration;
}

bool online(uint32_t now) {
    return hostSeen && !elapsed(now, hostAt, 4200);
}

bool hasCaption(uint32_t now) {
    return !caption.isEmpty() && !elapsed(now, captionAt, captionDuration);
}

unsigned visibleCaptionGlyphs(uint32_t now) {
    return 1 + uint32_t(now - captionPage.started) / kCharacterMs;
}

void show(Mood face, const String& words, uint32_t now, uint32_t duration = 4800) {
    mood = face;
    caption = words;
    captionAt = now;
    captionDuration = duration;
    captionPage.ready = false;
}

Mood parseMood(const String& value) {
    if (value == "happy") return Mood::Happy;
    if (value == "annoyed") return Mood::Annoyed;
    if (value == "sleepy") return Mood::Sleepy;
    if (value == "dizzy") return Mood::Dizzy;
    if (value == "surprised") return Mood::Surprised;
    if (value == "sad") return Mood::Sad;
    return Mood::Idle;
}

void stopAudio() {
    if (phase == Phase::Recording || phase == Phase::Finishing) M5.Mic.end();
    if (phase == Phase::Playing) {
        M5.Speaker.stop();
        M5.Speaker.end();
    }
    phase = Phase::Idle;
    audioId = "";
    pendingCaption = "";
    voiceSize = voiceReceived = 0;
    voiceDraining = false;
}

void finishPlayback(uint32_t now) {
    Serial.printf("PLAYED|%s|%lu\n", audioId.c_str(), static_cast<unsigned long>(now - phaseAt));
    stopAudio();
    // Let the last page remain readable after the sound ends.
    const auto visibleFor = uint32_t(now - captionAt);
    if (captionDuration < visibleFor + 1800) captionDuration = visibleFor + 1800;
}

void showPendingReply(uint32_t now, uint32_t duration = 7400) {
    show(pendingMood, pendingCaption, now, duration);
    Serial.printf("SHOWN|%s|%s\n", audioId == "ambient" ? "ambient" : "reply", audioId.c_str());
    pendingCaption = "";
}

void releasedMic(void*, void* data, size_t length) {
    micFilled.store(static_cast<int16_t*>(data) - mic + length, std::memory_order_release);
}

void beginRecording(uint32_t now) {
    stopAudio();
    lastAction = now;
    if (!online(now)) {
        show(Mood::Sad, "电脑还没连上，本鱼先陪你发呆。", now);
        return;
    }
    M5.Speaker.end();
    if (!mic || !M5.Mic.begin()) {
        show(Mood::Sad, "咦，麦克风没醒过来。", now);
        return;
    }
    ++turn;
    micFilled.store(0, std::memory_order_relaxed);
    micQueued = micSent = 0;
    phase = Phase::Recording;
    phaseAt = now;
    show(Mood::Surprised, "本鱼听着呢…", now, 12000);
}

void sendMicBlock(const uint8_t* data, size_t length) {
    constexpr char hex[] = "0123456789abcdef";
    char line[800];
    const int prefix = snprintf(line, sizeof(line), "MIC|%u|", turn);
    for (size_t i = 0; i < length; ++i) {
        line[prefix + i * 2] = hex[data[i] >> 4];
        line[prefix + i * 2 + 1] = hex[data[i] & 15];
    }
    line[prefix + length * 2] = '\n';
    Serial.write(reinterpret_cast<uint8_t*>(line), prefix + length * 2 + 1);
}

void finishRecording(uint32_t now) {
    phase = Phase::Finishing;
    phaseAt = now;
}

void audioTick(uint32_t now) {
    if (phase == Phase::Recording) {
        if (micQueued < kMicFrames && M5.Mic.isRecording() < 2) {
            if (M5.Mic.record(mic + micQueued * kMicFrame, kMicFrame, kMicRate)) ++micQueued;
        }
        if (elapsed(now, phaseAt, 12000)) finishRecording(now);
    }
    if (phase == Phase::Finishing &&
        (micFilled.load(std::memory_order_acquire) == micQueued * kMicFrame ||
         elapsed(now, phaseAt, 250))) {
        M5.Mic.end();
        const size_t samples = micFilled.load(std::memory_order_acquire);
        if (samples < kMicRate / 5) {
            stopAudio();
            show(Mood::Surprised, "按住按钮再说，本鱼才听得见。", now);
            return;
        }
        Serial.printf("REC|%u|%u\n", turn, static_cast<unsigned>(samples * 2));
        phase = Phase::Sending;
        phaseAt = now;
        show(Mood::Idle, "唔…让本鱼想一想。", now, 78000);
    }
    if (phase == Phase::Sending) {
        const size_t total = micFilled.load(std::memory_order_acquire) * 2;
        if (micSent < total) {
            const size_t count = min(size_t(360), total - micSent);
            sendMicBlock(reinterpret_cast<uint8_t*>(mic) + micSent, count);
            micSent += count;
        } else {
            Serial.printf("DONE|%u\n", turn);
            phase = Phase::Thinking;
            phaseAt = now;
        }
    }
    if (phase == Phase::Receiving && (elapsed(now, phaseAt, 3500) || !online(now))) {
        Serial.printf("AUDIOERR|%s|transfer\n", audioId.c_str());
        showPendingReply(now);
        stopAudio();
    } else if ((phase == Phase::Thinking && elapsed(now, phaseAt, 78000)) ||
               (phase != Phase::Idle && phase != Phase::Playing && !online(now))) {
        stopAudio();
        show(Mood::Sad, "没接上呢，等一下再试吧。", now);
    }
    if (phase == Phase::Playing && !M5.Speaker.isPlaying()) {
        if (!voiceDraining) {
            voiceDraining = true;
            voiceDrainedAt = now;
        }
        // The driver can still have audio in DMA after it releases the buffer.
        if (elapsed(now, voiceDrainedAt, 120)) finishPlayback(now);
    }
}

void gestureTick(uint32_t now) {
    if (!hasImu || !elapsed(now, lastImu, 20)) return;
    lastImu = now;
    if (!M5.Imu.update()) return;
    const auto sample = M5.Imu.getImuData();
    const auto a = sample.accel;
    const auto g = sample.gyro;
    const float magnitude = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    const float spin = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
    if (uprightAxis < 0 && magnitude > 0.82f && magnitude < 1.18f && spin < 20) {
        uprightAxis = fabsf(a.x) > fabsf(a.y) ? 0 : 1;
        if (fabsf(a.z) > fabsf(a.value[uprightAxis])) uprightAxis = 2;
        uprightSign = a.value[uprightAxis] < 0 ? -1 : 1;
    }
    shakeEnergy = shakeEnergy * 0.68f + fabsf(magnitude - 1.0f) * 0.32f;
    if (phase != Phase::Idle) return;
    if (uprightAxis >= 0 && magnitude > 0.76f && magnitude < 1.24f) {
        const float tilt = a.value[uprightAxis] * uprightSign;
        if (tilt < -0.72f && !inverted) {
            if (!tiltStarted) tiltStarted = now;
            if (elapsed(now, tiltStarted, 450)) {
                inverted = true;
                lastAction = gestureAt = now;
                show(Mood::Surprised, "诶？今天要倒着游吗？", now);
            }
        } else {
            tiltStarted = 0;
            if (tilt > 0.55f) inverted = false;
        }
    }
    if ((shakeEnergy > 0.24f || spin > 370) && elapsed(now, gestureAt, 2200)) {
        lastAction = gestureAt = now;
        show(Mood::Dizzy, "呜哇…本鱼的头晕晕的！", now);
    }
}

void buttons(uint32_t now) {
    if (M5.BtnA.wasPressed()) beginRecording(now);
    if (!M5.BtnA.isPressed() && phase == Phase::Recording) finishRecording(now);

    if (M5.BtnB.wasPressed()) {
        petHeld = true;
        petLong = false;
        petAt = now;
        lastAction = now;
    }
    if (petHeld && !petLong && elapsed(now, petAt, 700)) {
        petLong = true;
        muted = !muted;
        if (phase == Phase::Idle) {
            show(Mood::Sleepy, muted ? "嘘…本鱼安静陪你。" : "好啦，可以来找本鱼。", now);
        }
    }
    if (petHeld && !M5.BtnB.isPressed()) {
        if (!petLong && phase == Phase::Idle) {
            show(Mood::Happy, "摸头可以…再摸一下也行。", now);
        }
        petHeld = false;
    }
}

int nibble(char x) {
    if (x >= '0' && x <= '9') return x - '0';
    if (x >= 'a' && x <= 'f') return x - 'a' + 10;
    return -1;
}

String field(const String& line, unsigned& start) {
    const int index = line.indexOf('|', start);
    const unsigned end = index < 0 ? line.length() : unsigned(index);
    String result = line.substring(start, end);
    start = index < 0 ? line.length() : end + 1;
    return result;
}

void handleLine(const String& line, uint32_t now) {
    unsigned start = 0;
    const String kind = field(line, start);
    if (kind == "PING") {
        night = field(line, start) == "1";
        hostAt = now;
        hostSeen = true;
        Serial.printf("STATE|%s|%d|1\n",
                      phase != Phase::Idle ? "busy" : hasCaption(now) ? "notice" : "idle", muted);
    } else if (kind == "EV" && phase == Phase::Idle && !hasCaption(now)) {
        const Mood face = parseMood(field(line, start));
        const bool chime = field(line, start) == "1";
        show(face, line.substring(start), now, 6100);
        Serial.println("SHOWN|event|0");
        if (chime && !muted && !night && M5.Speaker.begin()) M5.Speaker.tone(880, 125);
    } else if (kind == "SAY") {
        const String id = field(line, start);
        const bool manual = phase == Phase::Thinking && id == String(turn);
        const bool ambient = id == "ambient" && phase == Phase::Idle &&
                             !hasCaption(now) && !muted && !night;
        if (!manual && !ambient) return;
        pendingMood = parseMood(field(line, start));
        const size_t bytes = field(line, start).toInt();
        audioId = id;
        pendingCaption = line.substring(start);
        if (voice && bytes && bytes <= kVoiceBytes && bytes % 2 == 0) {
            voiceSize = bytes;
            voiceReceived = 0;
            phase = Phase::Receiving;
            phaseAt = now;
        } else {
            showPendingReply(now);
            stopAudio();
        }
    } else if (kind == "A" && phase == Phase::Receiving) {
        if (field(line, start) != audioId) return;
        const size_t offset = field(line, start).toInt();
        const size_t chars = line.length() - start;
        if (offset != voiceReceived || chars % 2 || chars / 2 > voiceSize - offset) return;
        for (size_t i = 0; i < chars; i += 2) {
            const int high = nibble(line[start + i]), low = nibble(line[start + i + 1]);
            if (high < 0 || low < 0) return;
            voice[offset + i / 2] = (high << 4) | low;
        }
        voiceReceived += chars / 2;
        phaseAt = now;
        Serial.printf("ACK|%s|%u\n", audioId.c_str(), static_cast<unsigned>(voiceReceived));
    } else if (kind == "END" && phase == Phase::Receiving && field(line, start) == audioId) {
        if (voiceReceived != voiceSize) return;
        if (M5.Speaker.begin() &&
            M5.Speaker.playRaw(reinterpret_cast<const int16_t*>(voice), voiceSize / 2, kVoiceRate)) {
            phase = Phase::Playing;
            phaseAt = millis();
            const auto duration = static_cast<uint32_t>(voiceSize * 500 / kVoiceRate);
            showPendingReply(phaseAt, max(uint32_t(7400), duration + 1800));
            Serial.printf("PLAY|%s|%u\n", audioId.c_str(), static_cast<unsigned>(voiceSize));
        } else {
            Serial.printf("AUDIOERR|%s|speaker\n", audioId.c_str());
            M5.Speaker.end();
            showPendingReply(millis());
            stopAudio();
        }
    } else if (kind == "CHECK") {
        Serial.printf("CHECK|%d|%d|%d|%d|%d|%d\n", int(M5.getBoard()), hasImu, M5.Mic.isEnabled(),
                      M5.Speaker.isEnabled(), mic != nullptr && voice != nullptr, scene.width() == kWidth);
        if (hasCaption(now) && captionPage.ready) {
            const unsigned total = captionPage.glyphs[0] + captionPage.glyphs[1];
            Serial.printf("CAPTION|%u|%u|%u\n", captionPage.end,
                          min(visibleCaptionGlyphs(now), total), total);
        }
    }
}

void serialTick(uint32_t now) {
    for (unsigned i = 0; i < 2500 && Serial.available(); ++i) {
        const char c = Serial.read();
        if (c == '\n') {
            if (!discardLine) handleLine(rx, now);
            discardLine = false;
            rx = "";
        } else if (c != '\r' && !discardLine) {
            if (rx.length() < 1200) rx += c;
            else discardLine = true;
        }
    }
}

unsigned portrait(Mood face, uint32_t now) {
    using namespace portraits;
    if (phase == Phase::Playing && (face == Mood::Idle || face == Mood::Happy) && now / 170 % 2) {
        return Talk;
    }
    if (phase == Phase::Recording) return Curious;
    if (phase == Phase::Thinking || phase == Phase::Sending) return Think;
    switch (face) {
        case Mood::Happy: return (now - captionAt < 550) ? Wave : Smile;
        case Mood::Annoyed: return Annoyed;
        case Mood::Sleepy: return Sleep;
        case Mood::Dizzy: return Dizzy;
        case Mood::Surprised: return Curious;
        case Mood::Sad: return Sad;
        default: return (now % 4100 < 155) ? Blink : Idle;
    }
}

unsigned glyphBytes(const String& text, unsigned pos) {
    const uint8_t lead = text[pos];
    return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
}

String fitLine(const String& text, unsigned& pos, unsigned& glyphs) {
    String result;
    glyphs = 0;
    while (pos < text.length()) {
        const unsigned chars = glyphBytes(text, pos);
        if (pos + chars > text.length()) break;
        const String next = result + text.substring(pos, pos + chars);
        if (!result.isEmpty() && scene.textWidth(next) > kWidth - 28) break;
        result = next;
        pos += chars;
        ++glyphs;
    }
    return result;
}

void prepareCaptionPage(unsigned start, uint32_t now) {
    captionPage.end = start;
    for (unsigned row = 0; row < 2; ++row) {
        captionPage.lines[row] = fitLine(caption, captionPage.end, captionPage.glyphs[row]);
    }
    captionPage.started = now;
    captionPage.ready = true;
}

void drawCaptionText(uint32_t now, uint16_t accent) {
    if (!captionPage.ready) prepareCaptionPage(0, captionAt);
    const unsigned pageGlyphs = captionPage.glyphs[0] + captionPage.glyphs[1];
    if (pageGlyphs && captionPage.end < caption.length() &&
        elapsed(now, captionPage.started, (pageGlyphs - 1) * kCharacterMs + kPageHoldMs)) {
        prepareCaptionPage(captionPage.end, now);
    }
    unsigned visible = visibleCaptionGlyphs(now);
    for (unsigned row = 0; row < 2; ++row) {
        const auto& line = captionPage.lines[row];
        const unsigned shown = min(visible, captionPage.glyphs[row]);
        unsigned end = 0;
        for (unsigned i = 0; i < shown; ++i) end += glyphBytes(line, end);
        scene.drawString(line.substring(0, end), 14, 99 + row * 15);
        visible -= shown;
    }
    if (captionPage.end < caption.length() && visible) {
        scene.fillTriangle(220, 123, 226, 123, 223, 126, accent);
    }
}

void render(uint32_t now) {
    if (!elapsed(now, nextFrame, 33)) return;
    nextFrame = now;
    const bool dialogue = hasCaption(now);
    const bool asleep = phase == Phase::Idle && !dialogue && elapsed(now, lastAction, 180000);
    const Mood visible = dialogue ? mood : asleep ? Mood::Sleepy : Mood::Idle;
    M5.Display.setBrightness(asleep ? 47 : 103);
    const auto ink = scene.color565(22, 34, 67);
    const auto cyan = scene.color565(138, 225, 241);
    const auto pale = scene.color565(222, 246, 255);
    const auto lavender = scene.color565(196, 183, 250);
    for (int y = 0; y < 135; y += 3) {
        const int depth = y / 3;
        scene.fillRect(0, y, kWidth, 3, scene.color565(15 + depth / 5, 28 + depth, 56 + depth));
    }
    for (int i = 0; i < 24; ++i) {
        const int x = (i * 53 + 13) % 238, y = (i * 37 + 19) % 113 + 12;
        scene.drawPixel(x, y, scene.color565(73, 127, 162));
    }
    for (int i = 0; i < 7; ++i) {
        const int x = (i * 43 + 12) % 228 + 5;
        const int y = 22 + (100 + i * 19 - int(now / (140 + i * 24) % 100)) % 100;
        scene.drawCircle(x, y, i % 3 + 2, scene.color565(80, 151, 183));
        scene.drawPixel(x - 1, y - 1, cyan);
    }
    scene.fillEllipse(120, dialogue ? 91 : 126, 48, 4, scene.color565(22, 54, 99));
    const int bob = asleep ? 0 : int(lroundf(sinf(now * 0.0034f) * 2.5f));
    const int x = (kWidth - portraits::Width) / 2;
    const int y = (dialogue ? 0 : 14) + bob;
    const auto* frame = portraits::Pixels[portrait(visible, now)];
    if (inverted && phase == Phase::Idle) {
        scene.pushImageRotateZoom(120, y + portraits::Height / 2, portraits::Width / 2,
            portraits::Height / 2, 180, 1, 1, portraits::Width, portraits::Height,
            frame, portraits::Key);
    } else {
        scene.pushImage(x, y, portraits::Width, portraits::Height, frame, portraits::Key);
    }
    scene.setFont(&fonts::efontCN_12);
    scene.setTextDatum(top_left);
    scene.setTextColor(pale);
    scene.drawString("大肥鱼", 10, 7);
    scene.fillRect(8, 22, 40, 1, scene.color565(72, 149, 179));
    scene.setTextDatum(top_right);
    scene.setTextColor(muted ? lavender : online(now) ? cyan : pale);
    const char* status = muted ? "勿扰" : online(now) ? "在线" : "离线";
    scene.drawString(status, 231, 7);
    if (asleep) scene.drawString("Z z", 184, 38);
    if (dialogue) {
        scene.fillRect(7, 93, 226, 37, ink);
        scene.drawRect(8, 94, 224, 35, cyan);
        scene.drawFastHLine(12, 96, 216, scene.color565(52, 79, 117));
        scene.setTextDatum(top_left);
        scene.setTextColor(pale);
        scene.setFont(&fonts::efontCN_14);
        drawCaptionText(now, lavender);
    }
    scene.pushSprite(0, 0);
}

}  // namespace

void setup() {
    auto config = M5.config();
    M5.begin(config);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(103);
    scene.setColorDepth(16);
    scene.createSprite(kWidth, kHeight);
    hasImu = M5.Imu.isEnabled();
    mic = static_cast<int16_t*>(heap_caps_malloc(kMicFrames * kMicFrame * 2, MALLOC_CAP_SPIRAM));
    voice = static_cast<uint8_t*>(heap_caps_malloc(kVoiceBytes, MALLOC_CAP_SPIRAM));
    M5.Mic.setBufferReleaseCallback(nullptr, releasedMic);
    M5.Speaker.setVolume(kVoiceVolume);
    Serial.setRxBufferSize(8192);
    Serial.setTxBufferSize(8192);
    Serial.setTxTimeoutMs(22);
    Serial.begin(921600);
    rx.reserve(1200);
    lastAction = millis();
}

void loop() {
    M5.update();
    serialTick(millis());
    const uint32_t now = millis();
    buttons(now);
    audioTick(now);
    gestureTick(now);
    render(now);
    delay(1);
}
