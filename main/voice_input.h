#pragma once

#include <string>

// Dictation session states (UI polls these)
typedef enum {
    VOICE_IDLE = 0,
    VOICE_CONNECTING_WIFI,
    VOICE_CONNECTING_SERVER,
    VOICE_WAIT_ACTIVATE,     // 6-digit code shown, waiting user to bind
    VOICE_ACTIVATING,        // polling /activate
    VOICE_LISTENING,         // streaming dictation to xiaozhi cloud
    VOICE_STOPPING,
    VOICE_ERROR,
} voice_state_t;

// Voice dictation (xiaozhi cloud ASR). A single background task owns
// WiFi-for-voice, OTP activation, the WebSocket and audio streaming. The UI
// (screen_voice) polls state() and drains recognized text via popStt().
class VoiceInput {
public:
    // Start a dictation session. Spawns a background task. Returns false
    // if a session is already running (or still stopping).
    bool start();
    // Async stop request (double-click exit). Task sends "listen stop",
    // closes the socket and releases the mic.
    void requestStop();
    bool isActive() const;

    voice_state_t state() const;
    std::string errorMessage() const;
    std::string activationCode() const;
    std::string activationMessage() const;
    std::string lastStt() const;   // last recognized chunk, for live preview

    // Drains the next recognized text chunk (returns false when empty).
    bool popStt(std::string &out);
    // Main-loop housekeeping: WiFi idle 5-min shutdown for the voice session.
    void update();
};

extern VoiceInput g_voice;
