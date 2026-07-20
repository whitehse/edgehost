/**
 * Browser TTS for libanim narration cues (Web Speech API).
 *
 * Hardening for Chromium quirks:
 *  - cancel() then immediate speak() often drops the utterance
 *  - speechSynthesis can stick in paused state (needs resume)
 *  - getVoices() is empty until voiceschanged
 *  - long utterances need periodic resume() on Chrome
 */

/**
 * Expand acronyms so speech engines are less likely to mangle plant terms.
 * @param {string} text
 */
export function textForSpeech(text) {
  if (!text) return "";
  let s = String(text);
  const pairs = [
    [/\bONTs\b/g, "O N Tees"],
    [/\bONT\b/g, "O N T"],
    [/\bOLTs\b/g, "O L Tees"],
    [/\bOLT\b/g, "O L T"],
    [/\bPON\b/g, "P O N"],
    [/\bGPON\b/g, "G P O N"],
    [/\bXGS-PON\b/gi, "X G S P O N"],
    [/\bOTDR\b/g, "O T D R"],
    [/\bRx\b/g, "receive"],
    [/\bTx\b/g, "transmit"],
    [/\bdB\b/g, "decibels"],
    [/\b1:N\b/g, "one to N"],
    [/\b1:32\b/g, "one to thirty two"],
    [/\b10\/90\b/g, "ten ninety"],
    [/\b2-port\b/gi, "two port"],
    [/\bTDMA\b/g, "T D M A"],
    [/\bMAC\b/g, "M A C"],
    [/\bBER\b/g, "B E R"],
    [/\bLOS\b/g, "loss of signal"],
  ];
  for (const [re, rep] of pairs) s = s.replace(re, rep);
  return s;
}

/**
 * @param {{
 *   enabled?: boolean,
 *   rate?: number,
 *   pitch?: number,
 *   volume?: number,
 *   pauseWhileSpeaking?: boolean,
 *   lang?: string,
 *   log?: (s: string) => void,
 *   onStatus?: (line: string) => void,
 * }} [opts]
 */
export function createNarrator(opts = {}) {
  const log = typeof opts.log === "function" ? opts.log : () => {};
  const onStatus =
    typeof opts.onStatus === "function" ? opts.onStatus : () => {};

  const synth =
    typeof window !== "undefined" && "speechSynthesis" in window
      ? window.speechSynthesis
      : null;

  let enabled = opts.enabled !== false;
  let rate = opts.rate != null ? Number(opts.rate) : 1.0;
  let pitch = opts.pitch != null ? Number(opts.pitch) : 1.0;
  let volume = opts.volume != null ? Number(opts.volume) : 1.0;
  let pauseWhileSpeaking = opts.pauseWhileSpeaking !== false;
  let lang = opts.lang || "en-US";

  let speaking = false;
  let lastText = "";
  let lastError = "";
  let voice = null;
  let voiceCount = 0;
  let speakToken = 0;
  let resumeTimer = null;
  /** @type {SpeechSynthesisUtterance|null} */
  let currentUtter = null;

  function notify() {
    onStatus(statusLine());
  }

  function pickVoice() {
    if (!synth) return null;
    const voices = synth.getVoices() || [];
    voiceCount = voices.length;
    if (!voices.length) return null;
    const prefer = [
      (v) =>
        v.lang &&
        v.lang.toLowerCase().startsWith("en") &&
        (v.localService || v.default),
      (v) => v.lang && v.lang.toLowerCase().startsWith("en"),
      (v) => v.default,
      () => true,
    ];
    for (const pred of prefer) {
      const hit = voices.find(pred);
      if (hit) return hit;
    }
    return voices[0];
  }

  function ensureVoices() {
    if (!synth) return;
    voice = pickVoice();
  }

  function startResumeWatchdog() {
    stopResumeWatchdog();
    /* Chrome stops mid-utterance unless resume() is polled. */
    resumeTimer = setInterval(() => {
      if (!synth || !enabled) return;
      try {
        if (synth.paused || synth.speaking) synth.resume();
      } catch {
        /* ignore */
      }
    }, 4000);
  }

  function stopResumeWatchdog() {
    if (resumeTimer) {
      clearInterval(resumeTimer);
      resumeTimer = null;
    }
  }

  if (synth) {
    ensureVoices();
    try {
      speechSynthesis.addEventListener("voiceschanged", () => {
        ensureVoices();
        log(
          "voiceschanged count=" +
            voiceCount +
            " pick=" +
            (voice ? voice.name : "none")
        );
        notify();
      });
    } catch {
      /* ignore */
    }
    /* Some engines only populate voices after a tick */
    setTimeout(() => {
      ensureVoices();
      notify();
    }, 250);
  }

  function available() {
    return !!synth;
  }

  function isSpeaking() {
    if (!synth) return false;
    return speaking || synth.speaking || synth.pending;
  }

  function shouldHoldTimeline() {
    return enabled && pauseWhileSpeaking && isSpeaking();
  }

  function cancel() {
    stopResumeWatchdog();
    speakToken++;
    currentUtter = null;
    speaking = false;
    if (!synth) return;
    try {
      synth.cancel();
    } catch {
      /* ignore */
    }
    try {
      synth.resume();
    } catch {
      /* ignore */
    }
  }

  /**
   * @param {string} text
   * @param {{ force?: boolean }} [speakOpts]
   * @returns {boolean}
   */
  function speak(text, speakOpts = {}) {
    if (!enabled) {
      lastError = "voice disabled";
      notify();
      return false;
    }
    if (!synth) {
      lastError = "speechSynthesis missing in this browser";
      notify();
      return false;
    }
    const cleaned = textForSpeech(text).trim();
    if (!cleaned) {
      lastError = "empty text";
      notify();
      return false;
    }
    if (!speakOpts.force && cleaned === lastText && isSpeaking()) {
      return true;
    }

    ensureVoices();
    lastText = cleaned;
    lastError = "";

    const token = ++speakToken;
    /* Chromium often drops speak() if it follows cancel() in the same turn. */
    try {
      synth.cancel();
    } catch {
      /* ignore */
    }
    speaking = false;

    const run = () => {
      if (token !== speakToken || !enabled) return;

      ensureVoices();
      try {
        if (synth.paused) synth.resume();
      } catch {
        /* ignore */
      }

      const u = new SpeechSynthesisUtterance(cleaned);
      u.rate = rate;
      u.pitch = pitch;
      u.volume = volume;
      u.lang = lang;
      if (voice) {
        u.voice = voice;
        /* Keep lang consistent with chosen voice when possible */
        if (voice.lang) u.lang = voice.lang;
      }

      u.onstart = () => {
        if (token !== speakToken) return;
        speaking = true;
        lastError = "";
        log("speak start: " + cleaned.slice(0, 60));
        startResumeWatchdog();
        notify();
      };
      u.onend = () => {
        if (token !== speakToken) return;
        speaking = false;
        currentUtter = null;
        stopResumeWatchdog();
        log("speak end");
        notify();
      };
      u.onerror = (ev) => {
        if (token !== speakToken) return;
        speaking = false;
        currentUtter = null;
        stopResumeWatchdog();
        const err = (ev && ev.error) || "error";
        /* "interrupted" / "canceled" are normal when seeking */
        if (err === "interrupted" || err === "canceled") {
          lastError = "";
        } else {
          lastError = String(err);
          log("speak error: " + err);
        }
        notify();
      };

      currentUtter = u;
      try {
        synth.speak(u);
        /* If engine starts paused, kick it */
        try {
          synth.resume();
        } catch {
          /* ignore */
        }
        /*
         * Detect silent failure: no onstart within 1.5s and not speaking.
         * Common when no system voices are installed (Linux).
         */
        setTimeout(() => {
          if (token !== speakToken || !enabled) return;
          if (!speaking && !synth.speaking && !synth.pending) {
            lastError =
              voiceCount === 0
                ? "no TTS voices installed (Linux: install speech-dispatcher + espeak-ng)"
                : "speech did not start (try Test voice, or another browser)";
            log(lastError);
            notify();
          }
        }, 1500);
      } catch (e) {
        speaking = false;
        lastError = String(e);
        log("speak throw: " + e);
        notify();
      }
    };

    setTimeout(run, 40);
    notify();
    return true;
  }

  function setEnabled(on) {
    enabled = !!on;
    if (!enabled) cancel();
    notify();
  }

  function setRate(r) {
    rate = Math.min(2, Math.max(0.5, Number(r) || 1));
    notify();
  }

  function setPauseWhileSpeaking(on) {
    pauseWhileSpeaking = !!on;
    notify();
  }

  function statusLine() {
    if (!synth) return "Voice: unavailable (no Web Speech API in this browser)";
    if (!enabled) return "Voice: off";
    const vname = voice ? voice.name : voiceCount ? "default" : "no voices yet";
    let s =
      "Voice: on · " +
      vname +
      " · " +
      voiceCount +
      " voice(s)" +
      (pauseWhileSpeaking ? " · pause while speaking" : " · free-run");
    if (speaking || (synth && synth.speaking)) s += " · speaking…";
    if (lastError) s += " · ERROR: " + lastError;
    return s;
  }

  function diagnostics() {
    return {
      hasApi: !!synth,
      enabled,
      voiceCount,
      voiceName: voice ? voice.name : "",
      speaking: isSpeaking(),
      lastError,
      lastText: lastText.slice(0, 80),
      paused: !!(synth && synth.paused),
      pending: !!(synth && synth.pending),
    };
  }

  /**
   * Speak a fixed test phrase from a click handler (best chance to unlock TTS).
   */
  function testVoice() {
    ensureVoices();
    return speak(
      "Voice check. Fiber documentation narration is working.",
      { force: true }
    );
  }

  notify();

  return {
    available,
    isSpeaking,
    shouldHoldTimeline,
    speak,
    cancel,
    setEnabled,
    setRate,
    setPauseWhileSpeaking,
    statusLine,
    diagnostics,
    testVoice,
    ensureVoices,
    get enabled() {
      return enabled;
    },
    get rate() {
      return rate;
    },
    get pauseWhileSpeaking() {
      return pauseWhileSpeaking;
    },
    get lastError() {
      return lastError;
    },
  };
}
