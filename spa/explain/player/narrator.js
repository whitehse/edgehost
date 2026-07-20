/**
 * Browser TTS for libanim narration cues (Web Speech API).
 * No server, no audio assets. Works in Chromium/Edge; Safari support varies.
 *
 * Usage:
 *   const n = createNarrator({ pauseWhileSpeaking: true });
 *   n.speak("Downstream light leaves the OLT…");
 *   // in rAF: if (n.shouldHoldTimeline()) return; // skip tick
 */

/**
 * Expand acronyms so speech engines are less likely to mangle plant terms.
 * @param {string} text
 */
export function textForSpeech(text) {
  if (!text) return "";
  let s = String(text);
  const pairs = [
    [/\bONTs\b/g, "O N Ts"],
    [/\bONT\b/g, "O N T"],
    [/\bOLTs\b/g, "O L Ts"],
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
 * }} [opts]
 */
export function createNarrator(opts = {}) {
  const log = typeof opts.log === "function" ? opts.log : () => {};
  const synth =
    typeof window !== "undefined" && window.speechSynthesis
      ? window.speechSynthesis
      : null;

  let enabled = opts.enabled !== false;
  let rate = opts.rate != null ? opts.rate : 1.0;
  let pitch = opts.pitch != null ? opts.pitch : 1.0;
  let volume = opts.volume != null ? opts.volume : 1.0;
  let pauseWhileSpeaking = opts.pauseWhileSpeaking !== false;
  let lang = opts.lang || "en-US";
  let speaking = false;
  let lastText = "";
  let voice = null;
  let voicesReady = false;

  function pickVoice() {
    if (!synth) return null;
    const voices = synth.getVoices() || [];
    if (!voices.length) return null;
    /* Prefer local English voices */
    const prefer = [
      (v) => v.lang && v.lang.toLowerCase().startsWith("en") && v.localService,
      (v) => v.lang && v.lang.toLowerCase().startsWith("en"),
      () => true,
    ];
    for (const pred of prefer) {
      const hit = voices.find(pred);
      if (hit) return hit;
    }
    return voices[0] || null;
  }

  function ensureVoices() {
    if (!synth) return;
    voice = pickVoice();
    if (voice) voicesReady = true;
  }

  if (synth) {
    ensureVoices();
    if (typeof speechSynthesis !== "undefined") {
      speechSynthesis.addEventListener("voiceschanged", () => {
        ensureVoices();
        log("voices ready: " + (voice ? voice.name : "none"));
      });
    }
  }

  function available() {
    return !!synth;
  }

  function isSpeaking() {
    return speaking || !!(synth && synth.speaking);
  }

  /** When true, host should not advance the animation timeline. */
  function shouldHoldTimeline() {
    return enabled && pauseWhileSpeaking && isSpeaking();
  }

  function cancel() {
    if (!synth) return;
    try {
      synth.cancel();
    } catch {
      /* ignore */
    }
    speaking = false;
  }

  /**
   * Speak narration. Cancels any in-flight utterance (seek / new cue).
   * @param {string} text
   * @param {{ force?: boolean }} [speakOpts]
   */
  function speak(text, speakOpts = {}) {
    if (!enabled || !synth || !text) return false;
    const cleaned = textForSpeech(text).trim();
    if (!cleaned) return false;
    if (!speakOpts.force && cleaned === lastText && isSpeaking()) return true;

    cancel();
    ensureVoices();

    const u = new SpeechSynthesisUtterance(cleaned);
    u.rate = rate;
    u.pitch = pitch;
    u.volume = volume;
    u.lang = lang;
    if (voice) u.voice = voice;

    u.onstart = () => {
      speaking = true;
    };
    u.onend = () => {
      speaking = false;
    };
    u.onerror = () => {
      speaking = false;
    };

    lastText = cleaned;
    try {
      synth.speak(u);
      return true;
    } catch (e) {
      log("speak failed: " + e);
      speaking = false;
      return false;
    }
  }

  function setEnabled(on) {
    enabled = !!on;
    if (!enabled) cancel();
  }

  function setRate(r) {
    rate = Math.min(2, Math.max(0.5, Number(r) || 1));
  }

  function setPauseWhileSpeaking(on) {
    pauseWhileSpeaking = !!on;
  }

  function statusLine() {
    if (!synth) return "Voice: unavailable (no Web Speech API)";
    if (!enabled) return "Voice: off";
    const v = voice ? voice.name : "default";
    return (
      "Voice: on · " +
      v +
      (pauseWhileSpeaking ? " · pause timeline while speaking" : " · free-run")
    );
  }

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
    get enabled() {
      return enabled;
    },
    get rate() {
      return rate;
    },
    get pauseWhileSpeaking() {
      return pauseWhileSpeaking;
    },
  };
}
