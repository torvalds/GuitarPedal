const SYSEX_CMD = {
    REQ_SCHEMA: 0x01,
    RES_SCHEMA: 0x02,
    PARAM_UPDATE: 0x03,
    SAVE_SCENE: 0x04,
    REQ_STATE: 0x05,
    ROUTING_ORDER: 0x08,
    DIAGNOSTIC: 0x09,
    IDENTITY: 0x0a,
    TELEMETRY: 0x0b
};

//
// Actions must match enum bind_action in bindings.h.  The controls come
// from the pedal - which ones exist depends on the board and on what is
// in its expression jack, so it is not ours to know.
//
const SYSEX_SET_BINDING = 0x0c;
const SYSEX_BINDINGS = 0x0d;

//
// The four rule tables.  A gesture is answered by the most specific
// level that mentions its control at all, and that level answers it
// completely - so a scene rule does not add to a global one, it
// replaces it for that control.
//
// SCENE and GLOBAL are settings.  EFFECTIVE is what those two and the
// defaults resolve to, and DEFAULT is what is compiled in; both are
// read-only, and the pedal refuses a write to either rather than
// pretending it worked.
//
const RULES_SCENE = 0;
const RULES_GLOBAL = 1;
const RULES_EFFECTIVE = 2;
const RULES_DEFAULT = 3;

const ACT = {
    NONE: 0, POT: 1, NEXT_POT: 2, RESET_POT: 3,
    SET_POT: 4, TOGGLE_POT: 5, BYPASS: 6, TUNER: 7, SCENE: 8
};

// A target effect of this means "whatever the knob is turning".
const BIND_FOLLOW = 0x7f;

//
// A target parameter is numbered the way the SysEx parameter write
// numbers it: 0 is the mix, 1..10 are the effect's own pots.  The mix
// is not a pot and has no schema entry, so it needs a definition to be
// drawn from - the same one the effect cards use for their own mix
// slider.
//
const MIX_POT_DEF = { name: 'Mix', curve: 'LINEAR', min: 0, max: 100, unit: '%' };

let CONTROLS = [];

//
// A control the pedal did not describe still has to draw as something,
// because a rule naming it is already on the screen.
//
function controlDef(id) {
    return CONTROLS.find(c => c.id === id)
        || { id, name: `Control ${id}`, kind: 'click' };
}

//
// 'target' is whether the action names a parameter, and 'follow' is
// whether that parameter may be "the knob's" rather than a named one.
// 'values' is how many numbers it carries.
//
const ACTIONS = [
    { v: ACT.NONE,       label: 'Nothing' },
    { v: ACT.POT,        label: 'Control', target: true },
    { v: ACT.NEXT_POT,   label: 'Next parameter' },
    { v: ACT.RESET_POT,  label: 'Reset to default',
      target: true, follow: true },
    { v: ACT.SET_POT,    label: 'Set value', target: true, values: 1 },
    { v: ACT.TOGGLE_POT, label: 'Toggle between', target: true, values: 2 },
    { v: ACT.BYPASS,     label: 'Bypass' },
    { v: ACT.TUNER,      label: 'Tuner' },
    { v: ACT.SCENE,      label: 'Load scene' }
];

function actionDef(v) {
    return ACTIONS.find(a => a.v === v) || ACTIONS[0];
}

//
// The pedal accepts any action on any control, because a useless
// binding is not a dangerous one and it has no business refusing
// something merely silly.  Making the silly thing hard to say is this
// end's job: a control that turns can only drive a parameter, and one
// that clicks can do anything except that.
//
function actionsFor(ctrl) {
    return ACTIONS.filter(a => controlDef(ctrl).kind === 'turn'
                          ? (a.v === ACT.NONE || a.v === ACT.POT)
                          : a.v !== ACT.POT);
}

//
// The rule table, flat and in the pedal's order.  A gesture appears in
// it as many times as it has things to do.
//
let pedalRules = [];
let haveRules = false;

//
// Every level the pedal reported, indexed by RULES_*.  The editor works
// on the scene's table; the rest are here so the UI can say what a
// control actually does and what it would do if this rule went away.
//
let rulesByLevel = [[], [], [], []];

let midiAccess = null;
let midiInput = null;
let midiOutput = null;

// UI Elements
const appTitleEl = document.getElementById('app-title');
const globalEnableEl = document.getElementById('global-enable');
const effectsContainer = document.getElementById('effects-container');

// Map of CC to its HTML input element for O(1) updates
const ccToElementMap = new Map();

let isGlobalEnabled = false;
let activePotCc = null;
let activePotDef = null;
//
// The pedal filters Control Change and Program Change by
// settings.midi_channel and lets SysEx through regardless.  Transmit on
// the wrong one and global bypass, the tuner toggle, scene loads and the
// reboot all go quietly nowhere, while parameter edits carry on working
// - which is a confusing thing to debug, so keep this in step with the
// pedal rather than assuming channel 1 forever.
//
let activeTransmitChannel = 0xB0;

// Which pot in the schema is the channel. Found rather than hardcoded,
// and found by 'base' - the effect header's filename - because that is
// a steadier handle than the display name (see issue 20).
let midiChannelRef = null;

//
// Which pot the pedal filters its incoming MIDI by, so the app can
// transmit on the same channel.
//
// The pedal says so - 'ROLE: CHANNEL:POT2' in the effect header, carried
// through in the schema.  This used to look for an effect whose filename
// was 'settings' and then a pot whose label read "MIDI Ch", so renaming
// either quietly stopped the app tracking the channel, and quietly is
// the word: the fallback is transmitting on channel 1, which works
// perfectly until somebody sets a channel.
//
// A pedal older than the role tag has no answer here, and gets the same
// channel 1 it would have got before. Nothing looks for the label any
// more, which is the point.
//
function findMidiChannelPot() {
    const idx = PEDAL_EFFECTS.findIndex(
        (e) => e.roles && e.roles.CHANNEL !== undefined);

    return idx < 0 ? null
         : { idx, effId: PEDAL_EFFECTS[idx].id,
             pot: PEDAL_EFFECTS[idx].roles.CHANNEL };
}

//
// Pot value 0 is Omni, 1..16 are Ch1..Ch16.  On Omni the pedal takes
// whatever arrives, so channel 1 is as good as any.
//
function setTransmitChannel(potVal) {
    activeTransmitChannel = 0xB0 | (potVal > 0 ? (potVal - 1) & 0x0F : 0);

    // Keep the dialog's copy showing the same thing. Assigning .value
    // fires nothing, so this can't come back round.
    const sel = document.getElementById('midi-channel-select');
    if (sel && parseInt(sel.value) !== potVal)
        sel.value = potVal;
}

// Called once the schema has been rendered: pick the channel up from the
// control, and follow it when it is changed here. Changes arriving from
// the pedal come through the PARAM_UPDATE case instead, because setting
// .value from script fires no event.
function bindMidiChannel() {
    midiChannelRef = findMidiChannelPot();
    if (!midiChannelRef)
        return;

    const el = ccToElementMap.get(`eff-${midiChannelRef.idx}-pot-${midiChannelRef.pot}`);
    if (!el)
        return;

    //
    // The dialog gets a second view of this pot, built from the same
    // schema enum so the names can't drift.  It is a view and not a
    // second setting: changing it drives the card's control, which is
    // the one wired to the pedal, so there is still exactly one place
    // that decides what gets sent.
    //
    const sel = document.getElementById('midi-channel-select');
    if (sel) {
        const names = PEDAL_EFFECTS[midiChannelRef.idx].pots[midiChannelRef.pot].enum || [];
        sel.innerHTML = '';
        names.forEach((name, i) => {
            const opt = document.createElement('option');
            opt.value = i;
            opt.textContent = name;
            sel.appendChild(opt);
        });
        sel.addEventListener('change', () => {
            el.value = sel.value;
            el.dispatchEvent(new Event('change'));
        });
    }

    el.addEventListener('change', () => setTransmitChannel(parseInt(el.value)));
    setTransmitChannel(parseInt(el.value));
}

// Tuner State
let isTunerMode = false;
let playSynth = false;
const tunerState = Array(9).fill().map(() => ({ note: 0, cents: 0, volume: 0 }));
const noteNames = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

// Web Audio API Synth
let audioCtx = null;
const synthVoices = {}; // ch -> { osc, gain, filter, note }

function getAudioContext() {
    if (!audioCtx) {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (audioCtx.state === 'suspended') {
        audioCtx.resume();
    }
    return audioCtx;
}

function midiNoteToFreq(note, cents = 0) {
    return 440 * Math.pow(2, (note - 69 + cents / 100) / 12);
}

function startNoteVoice(ch, note, cents) {
    if (!playSynth) return;
    const ctx = getAudioContext();
    if (synthVoices[ch]) stopNoteVoice(ch);

    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    const filter = ctx.createBiquadFilter();

    osc.type = 'sawtooth';
    osc.frequency.value = midiNoteToFreq(note, cents);

    filter.type = 'lowpass';
    filter.frequency.value = 1500;

    osc.connect(filter);
    filter.connect(gain);
    gain.connect(ctx.destination);

    gain.gain.setValueAtTime(0, ctx.currentTime);
    gain.gain.linearRampToValueAtTime(0.1, ctx.currentTime + 0.05);

    osc.start();
    synthVoices[ch] = { osc, gain, filter, note };
}

function updateNoteVoiceBend(ch, note, cents) {
    if (synthVoices[ch] && synthVoices[ch].note === note) {
        const ctx = getAudioContext();
        synthVoices[ch].osc.frequency.setTargetAtTime(midiNoteToFreq(note, cents), ctx.currentTime, 0.05);
    }
}

function updateNoteVoiceVolume(ch, volume) {
    if (synthVoices[ch]) {
        const ctx = getAudioContext();
        // map 0-127 to a reasonable gain max, e.g., 0.2
        const targetGain = (volume / 127.0) * 0.2;
        synthVoices[ch].gain.gain.setTargetAtTime(targetGain, ctx.currentTime, 0.05);
    }
}

function stopNoteVoice(ch) {
    if (synthVoices[ch]) {
        const { osc, gain } = synthVoices[ch];
        const ctx = getAudioContext();
        gain.gain.cancelScheduledValues(ctx.currentTime);
        gain.gain.setValueAtTime(gain.gain.value, ctx.currentTime);
        gain.gain.linearRampToValueAtTime(0, ctx.currentTime + 0.1);
        osc.stop(ctx.currentTime + 0.15);
        delete synthVoices[ch];
    }
}

function stopAllNotes() {
    for (let ch in synthVoices) stopNoteVoice(ch);
}

// Initialize MIDI
async function initMidi() {
    try {
        console.log("[WebMIDI] Requesting MIDI access...");
        //
        // Missing entirely, which is not the same as refused, and there
        // are two quite different reasons for it.
        //
        // Web MIDI is only exposed on a secure origin, and http:// to a
        // LAN address is not one - which is exactly how this gets tested
        // from a phone.  That doesn't fail the call, it removes the API,
        // so the SecurityError branch below never sees it and the honest
        // "HTTPS Required" was unreachable.  Blaming the browser sends
        // you off checking the one thing that isn't wrong.
        //
        if (!navigator.requestMIDIAccess) {
            const insecure = !window.isSecureContext;
            appTitleEl.textContent = insecure ? "HTTPS Required" : "Browser Not Supported";
            console.error(insecure
                ? "Web MIDI needs a secure origin: https, localhost, or this origin allowed in chrome://flags/#unsafely-treat-insecure-origin-as-secure"
                : "Web MIDI API is not supported in this browser.");
            return;
        }
        midiAccess = await navigator.requestMIDIAccess({ sysex: true });
        midiAccess.onstatechange = updateMidiState;
        updateMidiState();
    } catch (err) {
        console.error("MIDI access denied", err);
        if (err.name === 'SecurityError') {
            appTitleEl.textContent = "HTTPS Required";
        } else if (err.name === 'NotAllowedError') {
            appTitleEl.textContent = "Permission Denied";
        } else if (err.name === 'InvalidStateError') {
            appTitleEl.textContent = "Tap to Connect";
        } else {
            appTitleEl.textContent = "MIDI Error: " + (err.name || "Unknown");
        }
    }
}

let selectedInputId = null;
let selectedOutputId = null;

//
// Which of the ports on offer is a pedal.
//
// The pedal names itself after the codec it found - "TAC5242 Pedal",
// "TAC5112 Pedal" - so the only part common to all of them is the word
// the match is now on.  As a word, so that something called "Pedalboard"
// is not mistaken for one.
//
// With two pedals plugged in both match and the first one wins.  That is
// what the selector below is for: auto-detect answers "a pedal", and
// saying which pedal is a choice only the person at the desk can make.
//
const PEDAL_PORT = /(^|\s)Pedal(\s|$)/;

function populateMidiSelects() {
    const inSelect = document.getElementById('midi-input-select');
    const outSelect = document.getElementById('midi-output-select');
    if (!inSelect || !outSelect) return;

    inSelect.innerHTML = '<option value="">-- Auto-detect pedal --</option>';
    outSelect.innerHTML = '<option value="">-- Auto-detect pedal --</option>';

    for (let input of midiAccess.inputs.values()) {
        const opt = document.createElement('option');
        opt.value = input.id;
        opt.textContent = input.name;
        if (input.id === selectedInputId) opt.selected = true;
        inSelect.appendChild(opt);
    }

    for (let output of midiAccess.outputs.values()) {
        const opt = document.createElement('option');
        opt.value = output.id;
        opt.textContent = output.name;
        if (output.id === selectedOutputId) opt.selected = true;
        outSelect.appendChild(opt);
    }
}

function updateMidiState() {
    let foundInput = null;
    let foundOutput = null;

    //
    // Every device, every time the MIDI state changes.  Useful when a
    // pedal is not showing up and noise the rest of the time, and it is
    // by far the most of what lands in the console - so it is debug
    // output, which browsers hide until asked.
    //
    console.debug("[WebMIDI] updating MIDI state. Available inputs:");
    for (let input of midiAccess.inputs.values()) {
        console.debug("  Input:", input.name, input.id);
    }
    console.debug("[WebMIDI] Available outputs:");
    for (let output of midiAccess.outputs.values()) {
        console.debug("  Output:", output.name, output.id);
    }

    if (selectedInputId && midiAccess.inputs.has(selectedInputId)) {
        foundInput = midiAccess.inputs.get(selectedInputId);
    } else {
        for (let input of midiAccess.inputs.values()) {
            if (!foundInput) foundInput = input;
            if (PEDAL_PORT.test(input.name)) {
                foundInput = input;
                break;
            }
        }
        if (foundInput && !selectedInputId) selectedInputId = foundInput.id;
    }

    if (selectedOutputId && midiAccess.outputs.has(selectedOutputId)) {
        foundOutput = midiAccess.outputs.get(selectedOutputId);
    } else {
        for (let output of midiAccess.outputs.values()) {
            if (!foundOutput) foundOutput = output;
            if (PEDAL_PORT.test(output.name)) {
                foundOutput = output;
                break;
            }
        }
        if (foundOutput && !selectedOutputId) selectedOutputId = foundOutput.id;
    }

    populateMidiSelects();

    if (foundInput && foundOutput) {
        if (midiInput !== foundInput) {
            midiInput = foundInput;
            midiInput.onmidimessage = handleMidiMessage;
        }
        midiOutput = foundOutput;
        appTitleEl.className = "title-connected";
        appTitleEl.textContent = `Connected: ${foundInput.name}`;

        // Who is this, then what does it have
        sendSysex([SYSEX_CMD.IDENTITY]);
        updateTelemetryPolling();
        sendSysex([SYSEX_CMD.REQ_SCHEMA]);
        sendSysex([SYSEX_CMD.DIAGNOSTIC]); // Request diagnostic status
    } else {
        midiInput = null;
        midiOutput = null;
        appTitleEl.className = "title-disconnected";
        appTitleEl.textContent = "RP2350 Pedal";

        // Nothing is going to tell us it stopped, so stop saying it
        clearPedalStatus();
        updateTelemetryPolling();
    }
}

//
// Are we the copy served off a laptop, or the deployed one?
//
// The two want to behave differently in small ways, and the difference
// was already being made once - index.html keeps the service worker
// away from a local page, because a stale worker outliving several
// reloads is a miserable way to spend an afternoon.  This is that same
// test, made once and shared, so the two can't drift apart.
//
// An empty hostname is a file:// URL.
//
// A private address counts as local too, because testing on a phone
// means serving to 192.168.x.x and the page is no less "the copy I am
// editing" for having crossed the room to get there.  That matters more
// than it looks: getting Web MIDI to work over http at all takes
// whitelisting the origin in Chrome's
// #unsafely-treat-insecure-origin-as-secure, and an origin secure
// enough for Web MIDI is secure enough to register a service worker.
// So without this the phone - the machine hardest to clear a stale
// worker out of - would be the one caching the dev server.
//
// The bracket is not optional: location.hostname keeps them on an IPv6
// literal, so requiring one is what stops this matching a real name that
// happens to start "fd".
//
const PRIVATE_HOST = /^(10\.|127\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.|\[(::1|fe80|f[cd]))|\.local$/i;

const IS_LOCAL_DEV = ['localhost', '127.0.0.1', '::1', ''].includes(location.hostname) ||
    PRIVATE_HOST.test(location.hostname);

// Colours the title amber rather than green - see style.css. The tooltip
// is set here rather than in the markup so the deployed copy doesn't
// carry an explanation of something it doesn't do.
if (IS_LOCAL_DEV) {
    document.body.classList.add('local-dev');
    if (appTitleEl)
        appTitleEl.title = 'Served locally — confirmations for destructive actions are skipped';
}

const updateAppBtn = document.getElementById('update-app-btn');
if (updateAppBtn) {
    updateAppBtn.addEventListener('click', async () => {
        if ('serviceWorker' in navigator) {
            const registrations = await navigator.serviceWorker.getRegistrations();
            for (let reg of registrations) {
                await reg.unregister();
            }
        }
        if ('caches' in window) {
            const keys = await caches.keys();
            for (let key of keys) {
                await caches.delete(key);
            }
        }
        window.location.reload();
    });
}


let diagnosticTimeout = null;
function scheduleDiagnostic() {
    if (diagnosticTimeout) clearTimeout(diagnosticTimeout);
    diagnosticTimeout = setTimeout(() => {
        sendSysex([SYSEX_CMD.DIAGNOSTIC]);
    }, 1000);
}

function sendSysex(data) {
    if (!midiOutput) return;
    const msg = new Uint8Array([0xF0, 0x7D, ...data, 0xF7]);
    midiOutput.send(msg);
    //
    // Having done something, ask whether it went wrong.  Queries are not
    // doing something, and telemetry especially is not: it is polled
    // several times a second, and dragging a diagnostic request along with
    // every frame would double the traffic and drain the pedal's one
    // status message before anybody could look at it.
    //
    const query = [SYSEX_CMD.DIAGNOSTIC, SYSEX_CMD.REQ_SCHEMA,
                   SYSEX_CMD.REQ_STATE, SYSEX_CMD.IDENTITY,
                   SYSEX_CMD.TELEMETRY];
    if (!query.includes(data[0]))
        scheduleDiagnostic();
}

let PEDAL_EFFECTS = [];

//
// In / Out / Merge, declared once by the pedal rather than per effect.
// Null against firmware that predates them, which is what every check
// of it is for.
//
let PEDAL_STEERING = null;
let effectIdMap = new Map();

//
// One pot value from the pedal, onto whatever draws it.
//
// Split out of the PARAM_UPDATE case so that a message carrying several
// pots applies them the same way it applies one.  Everything here is
// per-pot; the effect id is the only thing shared across a message.
//
function applyPotValue(effId, potIdx, val) {
    // The pedal telling us its channel changed - on a scene
    // load, say. Do this before the element update below, which
    // sets .value from script and so fires nothing.
    if (midiChannelRef && effId === midiChannelRef.effId &&
        potIdx === midiChannelRef.pot + 1)
        setTransmitChannel(val);

    const idx = effectIdMap.get(effId);
    if (idx === undefined) return;

    const idKey = potIdx === 0 ? `eff-${idx}-mix` : `eff-${idx}-pot-${potIdx-1}`;
    const el = ccToElementMap.get(idKey);
    if (!el) return;

    if (el.type === 'checkbox') {
        el.checked = (val > 0);
    } else if (el.tagName === 'SELECT') {
        el.value = val;
    } else if (el.type === 'range') {
        el.value = val;
        const valDisplay = el.parentElement.querySelector('.pot-value');
        if (valDisplay && el.potDef) {
            valDisplay.textContent = formatPotValue(el.potDef, val);
        }
        if (el.redrawCurve) {
            el.redrawCurve();
        }
        if (activePotDef && activePotCc === idKey) { // activePotCc is now acting as string key
            const activeSlider = document.getElementById('active-pot-slider');
            if (activeSlider) activeSlider.value = val;
            const activeValue = document.getElementById('active-pot-value');
            if (activeValue) activeValue.textContent = formatPotValue(activePotDef, val);
        }
    }
}

function handleSysex(data) {
    const cmd = data[2];
    console.debug(`[WebMIDI] Received SysEx cmd=0x${cmd.toString(16)}, data=[${Array.from(data).map(b => '0x' + b.toString(16).padStart(2, '0')).join(', ')}]`);

    switch (cmd) {
        case SYSEX_CMD.RES_SCHEMA: {
            // Schema Response
            let jsonStr = '';
            for (let i = 3; i < data.length - 1; i++) {
                jsonStr += String.fromCharCode(data[i]);
            }
            try {
                //
                // The schema used to be a bare array of effects and is
                // now an object with the effects under a key, alongside
                // the steering parameters every effect shares.  Both
                // shapes are accepted: a cached copy of this app can
                // meet firmware older than itself, and a service worker
                // makes that likelier than it sounds.
                //
                const parsed = JSON.parse(jsonStr);
                PEDAL_EFFECTS = Array.isArray(parsed) ? parsed : parsed.effects;
                PEDAL_STEERING = Array.isArray(parsed) ? null : parsed.steering;
                effectIdMap.clear();
                PEDAL_EFFECTS.forEach((e, idx) => effectIdMap.set(e.id, idx));
                renderUI();
                bindMidiChannel();
                // Request State and Status
                sendSysex([SYSEX_CMD.REQ_STATE]);
                sendSysex([SYSEX_CMD.DIAGNOSTIC]);
            } catch (e) {
                console.error("Failed to parse schema", e);
            }
            break;
        }

        case SYSEX_CMD.IDENTITY: {
            let jsonStr = '';
            for (let i = 3; i < data.length - 1; i++)
                jsonStr += String.fromCharCode(data[i]);
            try {
                const id = JSON.parse(jsonStr);
                //
                // Log the whole thing rather than the fields we happen
                // to render. It is a handful of bytes once per connect,
                // and it means a field added to answer a question on the
                // bench is visible without the app having to learn it
                // first.
                //
                console.log('[Pedal Identity]', id);
                handleIdentity(id);
            } catch (e) {
                console.error("Failed to parse identity", e);
            }
            break;
        }

        case SYSEX_CMD.TELEMETRY:
            handleTelemetry(data);
            break;

        case SYSEX_CMD.DIAGNOSTIC: {
            // Diagnostic Response
            let jsonStr = '';
            for (let i = 3; i < data.length - 1; i++) {
                jsonStr += String.fromCharCode(data[i]);
            }
            if (jsonStr.trim() !== '') {
                console.log(`[Pedal Diagnostic] ${jsonStr.trim()}`);
            }
            break;
        }

        case SYSEX_CMD.PARAM_UPDATE: {
            //
            // Set Parameter: one effect, then as many (pot, value)
            // pairs as the message carries.
            //
            // A message with one pair is byte for byte the message this
            // used to be, so nothing had to change about what the pedal
            // sends when a knob moves - only about what a state dump is
            // allowed to pack into one message.
            //
            if (data.length < 6) break;
            const effId = data[3];

            for (let at = 4; at + 1 < data.length; at += 2)
                applyPotValue(effId, data[at], data[at + 1]);
            break;
        }

        case SYSEX_BINDINGS: {
            const level = data[3];
            const list = [];

            for (let at = 4; at + 5 < data.length; at += 6) {
                list.push({ control: data[at],
                            action: data[at + 1],
                            effect: data[at + 2],
                            pot: data[at + 3],
                            val: [data[at + 4], data[at + 5]] });
            }
            rulesByLevel[level] = list;

            //
            // The editor still edits one table, and it is the scene's.
            // The other three are what it draws around that: what a
            // control ends up doing, and what it would fall back to.
            //
            if (level === RULES_SCENE) {
                pedalRules = list;
                haveRules = true;
            }

            //
            // A write is echoed at the level it was written, but it
            // changes what the control ends up doing as well - and that
            // is the table drawn for anything the scene is silent
            // about.  Without asking again, removing a rule leaves the
            // old one on screen as an inherited row: greyed, doing
            // nothing, and looking like it survived.
            //
            if (level === RULES_SCENE || level === RULES_GLOBAL)
                sendSysex([SYSEX_BINDINGS, RULES_EFFECTIVE]);

            renderBindings();
            renderKnobHint();
            break;
        }

        // Raw eeprom cache, 64 bytes as ASCII hex. Ask for it with
        // dumpEeprom(n) from the console; n counts 64-byte blocks.
        case 0x0f: {
            let hex = '';
            for (let i = 4; i < data.length - 1; i++)
                hex += String.fromCharCode(data[i]);
            const off = data[3] * 64;
            const bytes = hex.match(/../g) || [];
            console.log(`[EEPROM ${off.toString(16).padStart(4, '0')}] ` +
                        bytes.join(' '));
            break;
        }

        case SYSEX_CMD.ROUTING_ORDER: {
            // Routing order
            const routeIds = [];
            for (let i = 3; i < data.length - 1; i++) {
                routeIds.push(data[i]);
            }
            applyRouting(routeIds);

            // A state dump always concludes with the routing order.
            // Fetch status now to pick up any "Sent state dump" or similar info.
            sendSysex([SYSEX_CMD.DIAGNOSTIC]);
            break;
        }
    }
}

function handleMidiMessage(event) {
    if (event.data[0] === 0xF0) {
        if (event.data[1] === 0x7D) handleSysex(event.data);
        else console.debug(`[WebMIDI] Unknown SysEx: [${Array.from(event.data).map(b => '0x' + b.toString(16).padStart(2, '0')).join(', ')}]`);
        return;
    }
    const [status, data1, data2] = event.data;
    console.debug(`[WebMIDI] Received normal MIDI message: status=0x${status ? status.toString(16) : 'undefined'}, data1=${data1}, data2=${data2}`);

    // Control Change (0xB0 to 0xBF, we just mask to 0xB0 for channel 1)
    if ((status & 0xF0) === 0xB0) {
        const cc = data1;
        const val = data2;

        if (cc === GLOBAL_ENABLE_CC) {
            if (val === 68 || val === 69) {
                isTunerMode = (val === 68);
                updateTunerModeUI();
            } else {
                isGlobalEnabled = (val > 0);
                globalEnableEl.checked = isGlobalEnabled;
            }
        } else if (cc === STATUS_GLOBAL_CC) {
            handleGlobalStatus(val);
        } else if (cc === STATUS_CHAIN_LO_CC) {
            statusChainBits = (statusChainBits & ~((1 << STATUS_CHAIN_BITS) - 1)) | val;
            renderAttention();
        } else if (cc === STATUS_CHAIN_HI_CC) {
            statusChainBits = (statusChainBits & ((1 << STATUS_CHAIN_BITS) - 1)) |
                              (val << STATUS_CHAIN_BITS);
            renderAttention();
        }
    } else if ((status & 0xF0) === 0x90) { // Note On
        const ch = status & 0x0F;
        const note = data1;
        const vel = data2;
        if (vel > 0) {
            if (ch < 9) {
                tunerState[ch].note = note;
                startNoteVoice(ch, note, tunerState[ch].cents);
            }
        } else {
            if (ch < 9) {
                tunerState[ch].note = 0;
                stopNoteVoice(ch);
            }
        }
        updateTunerDisplay();
    } else if ((status & 0xF0) === 0x80) { // Note Off
        const ch = status & 0x0F;
        if (ch < 9) {
            tunerState[ch].note = 0;
            stopNoteVoice(ch);
        }
        updateTunerDisplay();
    } else if ((status & 0xF0) === 0xE0) { // Pitch Bend
        const ch = status & 0x0F;
        const bend = data1 | (data2 << 7);
        if (ch < 9) {
            tunerState[ch].cents = Math.round((bend - 8192) / 41);
            if (tunerState[ch].note > 0) {
                updateNoteVoiceBend(ch, tunerState[ch].note, tunerState[ch].cents);
            }
        }
        updateTunerDisplay();
    } else if ((status & 0xF0) === 0xD0) { // Channel Pressure
        const ch = status & 0x0F;
        const pressure = data1;
        if (ch < 9) {
            tunerState[ch].volume = pressure;
            if (tunerState[ch].note > 0) {
                updateNoteVoiceVolume(ch, pressure);
            }
        }
        updateTunerDisplay();
    }
}

function updateTunerModeUI() {
    const tunerBtn = document.getElementById('tuner-btn');
    const tunerPanel = document.getElementById('tuner-panel');
    if (tunerBtn && tunerPanel) {
        if (isTunerMode) {
            tunerBtn.classList.add('active');
            tunerPanel.classList.remove('hidden');
        } else {
            tunerBtn.classList.remove('active');
            tunerPanel.classList.add('hidden');
        }
    }
}

function updateTunerDisplay() {
    if (!isTunerMode) return;

    const chrom = tunerState[0];
    const chromNoteEl = document.getElementById('tuner-chromatic-note');
    const chromCentsEl = document.getElementById('tuner-chromatic-cents');
    const chromNeedle = document.getElementById('tuner-needle');

    if (chromNoteEl && chromNeedle) {
        if (chrom.note) {
            const name = noteNames[chrom.note % 12];
            const octave = Math.floor(chrom.note / 12) - 1;
            chromNoteEl.textContent = `${name}${octave}`;

            if (chromCentsEl) {
                const sign = chrom.cents > 0 ? '+' : '';
                chromCentsEl.textContent = `${sign}${chrom.cents}¢`;
            }

            let leftPercent = 50 + chrom.cents;
            leftPercent = Math.max(0, Math.min(100, leftPercent));
            chromNeedle.style.left = `${leftPercent}%`;
            chromNeedle.style.display = 'block';

            if (Math.abs(chrom.cents) < 5) {
                chromNeedle.style.backgroundColor = 'var(--success)';
            } else {
                chromNeedle.style.backgroundColor = 'var(--danger)';
            }

            const chromVolEl = document.getElementById('tuner-chromatic-vol');
            if (chromVolEl) chromVolEl.textContent = `V: ${chrom.volume}`;
        } else {
            chromNoteEl.textContent = '--';
            if (chromCentsEl) chromCentsEl.textContent = '0¢';
            chromNeedle.style.display = 'none';
            const chromVolEl = document.getElementById('tuner-chromatic-vol');
            if (chromVolEl) chromVolEl.textContent = `V: 0`;
        }
    }

    const polyContainer = document.getElementById('tuner-poly-strings');
    if (polyContainer) {
        if (polyContainer.children.length === 0) {
            for (let i = 1; i <= 6; i++) {
                const strDiv = document.createElement('div');
                strDiv.className = 'tuner-string';
                strDiv.innerHTML = `
                    <div class="tuner-string-name" id="str-name-${i}">-</div>
                    <div class="tuner-string-arrow inactive" id="str-arrow-${i}"></div>
                    <div class="tuner-string-vol" id="str-vol-${i}">0</div>
                `;
                polyContainer.appendChild(strDiv);
            }
        }

        for (let i = 1; i <= 6; i++) {
            const state = tunerState[i];
            const nameEl = document.getElementById(`str-name-${i}`);
            const arrowEl = document.getElementById(`str-arrow-${i}`);
            const volEl = document.getElementById(`str-vol-${i}`);

            if (state && state.note) {
                nameEl.textContent = noteNames[state.note % 12];
                nameEl.classList.add('active');
                arrowEl.className = 'tuner-string-arrow';
                if (state.cents > 10) arrowEl.classList.add('down');
                else if (state.cents < -10) arrowEl.classList.add('up');
                else arrowEl.classList.add('perfect');
                if (volEl) volEl.textContent = state.volume;
            } else {
                nameEl.classList.remove('active');
                arrowEl.className = 'tuner-string-arrow inactive';
                if (volEl) volEl.textContent = "0";
            }
        }
    }
}


function sendMidiPc(pc) {
    if (!midiOutput) return;
    midiOutput.send([0xC0 | (activeTransmitChannel & 0x0F), pc]);
    scheduleDiagnostic();
}

function sendMidiCc(cc, val) {
    if (!midiOutput) return;
    midiOutput.send([activeTransmitChannel, cc, val]);
    scheduleDiagnostic();
}

//
// Let the scroll wheel adjust whichever slider it is over.
//
// The catch is that wheel events go to whatever happens to be under the
// pointer, and the pointer doesn't move while you scroll - the page does.
// So a naive version means that scrolling past a slider hands it the
// wheel, and since the page keeps moving underneath, you end up dragging
// one value after another while trying to get down the list. Every one
// of those goes straight to the pedal.
//
// A scroll gesture has a beginning, though, so settle it there: whatever
// was under the pointer when the gesture started owns the whole gesture.
// Start on a slider and it's yours until you pause; start anywhere else
// and sliders sliding past underneath are ignored.
//
const WHEEL_NOTCH = 100;        // about one detent of a mouse wheel
const WHEEL_GESTURE_GAP = 400;  // ms of quiet before it counts as a new one
const WHEEL_STEP = 3;           // pot units per detent - 40 of them end to end
const WHEEL_STEP_FINE = 1;      // ...and with shift held

const wheelZones = new Map();   // hoverable area -> the slider it drives
let wheelOwner = null;
let wheelLast = 0;

function wheelZoneAt(el) {
    for (; el; el = el.parentElement)
        if (wheelZones.has(el))
            return el;
    return null;
}

// Capture, so this settles ownership before the zone's own handler runs
window.addEventListener('wheel', (e) => {
    if (e.timeStamp - wheelLast > WHEEL_GESTURE_GAP) {
        wheelOwner = wheelZoneAt(e.target);

        //
        // Move the focus to whatever the wheel just took charge of.
        //
        // The ring is the only thing on screen saying which slider is
        // about to move, and it used to follow clicks while the wheel
        // followed the pointer - so clicking one and then scrolling
        // over another left it pointing at the wrong control.  One
        // notion of "the active pot" rather than two, and the arrow
        // keys now act on whatever the wheel last did.
        //
        // preventScroll because the default is to scroll the element
        // into view, and doing that in the middle of a scroll would
        // fight the user for the thing they are actually doing.
        //
        const input = wheelOwner && wheelZones.get(wheelOwner);
        if (input && input !== document.activeElement)
            input.focus({ preventScroll: true });
    }
    wheelLast = e.timeStamp;
}, { passive: true, capture: true });

//
// 'zone' is the area the wheel answers over.  It wants to be the whole
// pot - name, readout and slider together - because aiming at the few
// pixels of the slider itself just reads as "it doesn't work".
//
function enableWheelAdjust(input, zone = input) {
    let acc = 0;

    wheelZones.set(zone, input);
    zone.addEventListener('wheel', (e) => {
        if (wheelOwner !== zone)
            return;
        e.preventDefault();

        // Accumulate, so that a trackpad's stream of small deltas adds
        // up to the same thing as one click of a wheel
        acc -= e.deltaY;                        // wheel up raises the value
        const notches = Math.trunc(acc / WHEEL_NOTCH);
        if (!notches)
            return;
        acc -= notches * WHEEL_NOTCH;

        const step = e.shiftKey ? WHEEL_STEP_FINE : WHEEL_STEP;
        const now = parseInt(input.value);
        const val = Math.max(parseInt(input.min),
                             Math.min(parseInt(input.max), now + notches * step));
        if (val === now)
            return;

        // Go through the normal 'input' path, so the readout, the eq
        // curve and the sysex all happen exactly as they would if you
        // had dragged it
        input.value = val;
        input.dispatchEvent(new Event('input'));
    }, { passive: false });
}

//
// The pot curves, once each way round.
//
// A pot is a number from 0 to 120 on the wire and something physical to
// look at - hertz, decibels, milliseconds - and which curve joins the
// two is declared in the effect's C header and comes through in the
// schema. gen_effects.py holds the same pair for the firmware's own
// table; these are the app's copy, and everything in the app goes
// through them.
//
// There were four. These two, and a second pair private to the
// parametric EQ that knew LINEAR and FREQUENCY and quietly handed back
// the raw pot value for anything else - so giving that effect a curve
// of a different kind would have drawn a flat line through 0 to 120 Hz
// and called it a response.
//
function clampPot(val) {
    return Math.max(0, Math.min(120, isNaN(val) ? 0 : val));
}

function potToValue(pot, val) {
    const p = val / 120.0;

    switch (pot.curve) {
    case 'LINEAR':      return pot.min + p * (pot.max - pot.min);
    case 'FREQUENCY':   return pot.min + p * p * p * (pot.max - pot.min);
    case 'SQUARED':     return pot.min + p * p * (pot.max - pot.min);
    case 'EXPONENTIAL': return pot.min * Math.pow(pot.max / pot.min, p);
    }
    return val;                 // RAW and ENUM are already the value
}

//
// And back. Rounding to a whole pot step is part of the answer rather
// than something for the caller to do afterwards: 121 values is all
// there is, so anything finer is a number the pedal cannot be told.
//
function valueToPot(pot, y) {
    if (pot.curve === 'RAW' || pot.curve === 'ENUM')
        return clampPot(Math.round(y));

    const a = pot.min, b = pot.max;
    let p = 0;

    // A ratio outside the declared range is the caller's business to
    // clamp; taking a cube root of a negative one here is not, and
    // neither is a logarithm of it
    if (b !== a) {
        const ratio = Math.max(0, (y - a) / (b - a));

        switch (pot.curve) {
        case 'LINEAR':      p = ratio; break;
        case 'FREQUENCY':   p = Math.cbrt(ratio); break;
        case 'SQUARED':     p = Math.sqrt(ratio); break;
        case 'EXPONENTIAL':
            p = (a > 0 && y > 0) ? Math.log2(y / a) / Math.log2(b / a) : 0;
            break;
        }
    }
    return clampPot(Math.round(p * 120));
}

//
// How the app behaves, as opposed to what the pedal does.
//
// Kept in localStorage because these belong to the person and the
// screen, not to the pedal: a scene must not be able to change how
// dragging works, and the pedal has no idea any of this exists. Which
// is also why they are not pots - there is nothing here to send.
//
function uiPref(key, fallback) {
    try {
        const val = localStorage.getItem('ui.' + key);
        return val === null ? fallback : val === 'true';
    } catch (err) {
        // Storage can be refused outright, and a preference is not
        // worth failing over
        return fallback;
    }
}

function setUiPref(key, val) {
    try {
        localStorage.setItem('ui.' + key, val ? 'true' : 'false');
    } catch (err) {
        /* then it lasts as long as the page does, which will do */
    }
}

//
// Whether the EQ's bands are held in order while you drag them.
//
// On by default, and a switch rather than a decision made for you: the
// unordered curves the old overlapping pot ranges allowed were odd but
// not wrong, and some of them are interesting. Turning this off gets
// them back.
//
// Nothing happens retroactively. Switching it on does not tidy up bands
// that are already crossed - it only governs what a drag is allowed to
// do, so an existing curve is left exactly as it was until you take
// hold of something.
//
let eqKeepOrder = uiPref('eq.keepOrder', true);

//
// Whether the phase response is drawn behind the magnitude.
//
// Dark grey on black, so it is there if you look for it and not
// otherwise.  Phase is worth being able to see - five sections in
// series is a lot of it, and the arrangements that look strange on the
// magnitude plot are where it is least obvious - but it is reference
// rather than the thing being edited, and the nodes and the curve stay
// the subject.
//
let eqShowPhase = uiPref('eq.showPhase', true);

//
// Keeping the EQ's bands in order.
//
// The pedal does not care - five biquads in series commute, and every
// band now has the whole audio range - so this is entirely a question
// about what a control should do, and it lives here rather than in the
// firmware.
//
// A band may travel as far as its neighbours and no further.  The
// alternative was to push the neighbours along ahead of it, which reads
// well until you drag the low shelf up to 10kHz and arrive to find it
// has taken the other four with it and flattened a curve you spent a
// while on.  Clamping never touches a band you did not grab, and it
// costs nothing: every ordered arrangement is still reachable, by
// moving the band that is in the way first, and unordered ones are what
// this is for.
//
// In pot units rather than hertz.  Pot units are what gets stored and
// what gets sent, so clamping there is exact - clamp in hertz and the
// conversion back can round to a step the far side of the neighbour it
// was just clamped to.
//
function clampToNeighbours(vals, idx, want) {
    const lo = idx > 0 ? vals[idx - 1] : 0;
    const hi = idx < vals.length - 1 ? vals[idx + 1] : 120;

    return Math.max(lo, Math.min(hi, want));
}

//
// Which bands are sitting exactly on top of this one.
//
// Clamping produces exact equality rather than near misses - a band
// stopped by its neighbour is given that neighbour's value - so a pile
// can be identified by that alone, with no tolerance to tune. Two bands
// that are merely close are not a pile, and go on being told apart by
// which one you aimed at.
//
function pileAt(vals, idx) {
    const pile = [];

    for (let i = 0; i < vals.length; i++)
        if (vals[i] === vals[idx])
            pile.push(i);

    return pile;
}

//
// A frequency, short enough to sit under a control point.
//
// Two significant figures, because that is about all the control can
// select.  The frequency pots step by 5.9%, so a third digit is a claim
// about precision that does not exist: "1.68kHz" was a reading of a
// number rather than a description of a setting, and the next position
// along would have been 1.78.
//
function formatFreqShort(freq) {
    const mag = Math.pow(10, 1 - Math.floor(Math.log10(freq)));
    const round = Math.round(freq * mag) / mag;

    if (round < 1000)
        return round.toFixed(0) + 'Hz';

    const k = round / 1000;
    return (k >= 10 ? k.toFixed(0) : k.toFixed(1)) + 'kHz';
}

function formatPotValue(pot, val) {
    const y = potToValue(pot, val);
    let displayStr = "";
    if (pot.curve === 'RAW' || pot.curve === 'ENUM') {
        displayStr = Math.round(y).toString();
    } else {
        // Drop trailing zeros, max 2 decimals
        displayStr = parseFloat(y.toFixed(2)).toString();
    }

    if (pot.unit && pot.unit !== 'none') {
        displayStr += ' ' + pot.unit;
    }
    return displayStr;
}


//
// Dragging effect cards around.
//
// This is done with pointer events rather than html5 drag-and-drop.
// html5 dnd is a file-transfer api from 2008 with a stateful 'draggable'
// attribute, drop events that only fire over registered targets, and no
// touch support worth the name - ios safari has none at all, so
// reordering simply did not work on a phone.  Pointer events are one
// code path for mouse, touch and pen, and capturing the pointer means
// every move and the release come to us no matter what ends up under
// the cursor.
//
// The card is moved in the list as you go, rather than dragging a
// floating copy about: it is less code, and the list ends up in exactly
// the state that routingFromDOM() then reads back out of the dom.
//
// Dragging reorders the chain and does nothing else.  It used to also be
// how an effect was routed and unrouted - drag it across a divider - and
// that was one gesture too many for it: on a phone it meant a long drag
// past every card you were not interested in, and it made card position
// the routing model.  Routing is buttons now, and this is left doing the
// one thing a drag is genuinely good at.
//
const DRAG_THRESHOLD = 4;       // px before a press counts as a drag
const DRAG_EDGE = 80;           // px from the edge where we start scrolling
const DRAG_SCROLL_MAX = 18;     // px per frame at the very edge

let cardDrag = null;
let dragScrollRaf = 0;

// Is this element a card that is currently in the chain?
function isRoutedCard(el) {
    if (!el.dataset || el.dataset.effectId === undefined)
        return false;
    return currentRouting.includes(parseInt(el.dataset.effectId));
}

//
// Which element should the gap sit in front of?
//
// Only chain cards are landmarks.  Everything else in the container -
// the anchor at the front, the settings card, the parked cards and the
// pool - is somewhere a dragged card may not go, so the search walks
// past them and the chain's own extent is what bounds the drag.  There
// is nothing to clamp and no divider to be on the wrong side of.
//
function dragInsertionRef(y) {
    let seenRouted = false;
    let end = null;

    for (const el of effectsContainer.children) {
        // The dragged card is out of flow and the gap is what we're
        // placing, so neither is a landmark
        if (el === cardDrag.card || el === cardDrag.gap)
            continue;

        if (!isRoutedCard(el)) {
            // The first thing past the chain is where a card dragged
            // below the last one lands
            if (seenRouted && end === null)
                end = el;
            continue;
        }
        seenRouted = true;

        const r = el.getBoundingClientRect();
        if (y < r.top + r.height / 2)
            return el;
    }

    // The only effect in the chain has nowhere to go, so leave the gap
    // where it already is rather than sending it to the bottom
    if (!seenRouted)
        return cardDrag.gap.nextSibling;

    return end;
}

// Take the card out of the flow and leave a gap the same size behind it,
// so it can follow the pointer instead of only jumping between slots
// when you cross a midpoint. The gap is what actually gets moved around
// the list, and it's also where the card goes back when you let go.
function dragLift() {
    const r = cardDrag.card.getBoundingClientRect();

    cardDrag.gap = document.createElement('div');
    cardDrag.gap.className = 'drag-placeholder';
    cardDrag.gap.style.height = r.height + 'px';
    effectsContainer.insertBefore(cardDrag.gap, cardDrag.card);

    cardDrag.grabX = cardDrag.x - r.left;
    cardDrag.grabY = cardDrag.y - r.top;

    cardDrag.card.style.width = r.width + 'px';
    cardDrag.card.classList.add('dragging');
    dragFollow();
}

// The card is position:fixed, so these are plain viewport coordinates -
// which is also why auto-scrolling slides the list underneath without
// the card drifting away from the pointer.
function dragFollow() {
    cardDrag.card.style.left = (cardDrag.x - cardDrag.grabX) + 'px';
    cardDrag.card.style.top = (cardDrag.y - cardDrag.grabY) + 'px';
}

function dragReposition() {
    const ref = dragInsertionRef(cardDrag.y);
    if (ref !== cardDrag.gap.nextSibling)
        effectsContainer.insertBefore(cardDrag.gap, ref);
}

function dragDrop() {
    cardDrag.card.classList.remove('dragging');
    cardDrag.card.style.width = '';
    cardDrag.card.style.left = '';
    cardDrag.card.style.top = '';
    effectsContainer.insertBefore(cardDrag.card, cardDrag.gap);
    cardDrag.gap.remove();
}

// Keep scrolling while the pointer is held near an edge, which matters
// on a phone where the list is a lot taller than the screen. Has to be
// on a timer rather than driven by pointermove, or holding still at the
// edge would stop it.
function dragScrollTick() {
    if (!cardDrag || !cardDrag.moved) {
        dragScrollRaf = 0;
        return;
    }
    const h = window.innerHeight;
    let dy = 0;

    if (cardDrag.y < DRAG_EDGE)
        dy = -DRAG_SCROLL_MAX * (1 - cardDrag.y / DRAG_EDGE);
    else if (cardDrag.y > h - DRAG_EDGE)
        dy = DRAG_SCROLL_MAX * (1 - (h - cardDrag.y) / DRAG_EDGE);

    if (dy) {
        window.scrollBy(0, dy);
        dragReposition();
    }
    dragScrollRaf = requestAnimationFrame(dragScrollTick);
}

function cardDragStart(card, grip, e) {
    if (!e.isPrimary || cardDrag)
        return;

    // The header carries controls of its own. A press that lands on one
    // of those belongs to it, not to a drag.
    if (e.target.closest('.collapse-chevron, .action-btn'))
        return;

    //
    // A finger starts a drag on the handle and nowhere else, so the rest
    // of the header is still somewhere the page can be scrolled from.
    // The handle is the only thing in the card with touch-action: none,
    // and this is the half of that rule that lives in script - see
    // .drag-handle in the stylesheet for the other half and for why.
    //
    if (e.pointerType !== 'mouse' && !e.target.closest('.drag-handle'))
        return;

    //
    // Capture stops the browser reinterpreting the gesture - a touch
    // that wanders off the header would otherwise become a scroll - but
    // don't rely on it lasting.  Repositioning the card moves the header
    // along with it, and moving the element that holds a capture
    // releases the capture, after which pointerup goes to whatever is
    // under the pointer instead of to us.
    //
    // So take the capture for the browser's benefit, and listen on the
    // window for ours.
    //
    try {
        grip.setPointerCapture(e.pointerId);
    } catch (err) {
        /* not fatal, the window listeners are what matter */
    }

    // Suppress selection from the press, not from the drag. Waiting for
    // the threshold is too late - a phone has put the selection UI up
    // by then. The header itself is user-select: none in the stylesheet;
    // this covers everything the pointer travels over afterwards.
    document.body.style.userSelect = 'none';

    cardDrag = { card, grip, id: e.pointerId, gap: null,
                 startY: e.clientY, x: e.clientX, y: e.clientY, moved: false };

    window.addEventListener('pointermove', cardDragMove);
    window.addEventListener('pointerup', cardDragEnd);
    window.addEventListener('pointercancel', cardDragEnd);
}

function cardDragMove(e) {
    if (!cardDrag || e.pointerId !== cardDrag.id)
        return;
    cardDrag.x = e.clientX;
    cardDrag.y = e.clientY;

    if (!cardDrag.moved) {
        // A press that never moves is a press, not a drag - otherwise
        // just clicking the handle would send a routing update.
        if (Math.abs(e.clientY - cardDrag.startY) < DRAG_THRESHOLD)
            return;
        cardDrag.moved = true;
        dragLift();
        if (!dragScrollRaf)
            dragScrollRaf = requestAnimationFrame(dragScrollTick);
    }
    dragFollow();
    dragReposition();
}

function cardDragEnd(e) {
    if (!cardDrag || e.pointerId !== cardDrag.id)
        return;
    const moved = cardDrag.moved;

    window.removeEventListener('pointermove', cardDragMove);
    window.removeEventListener('pointerup', cardDragEnd);
    window.removeEventListener('pointercancel', cardDragEnd);

    if (moved)
        dragDrop();
    cardDrag = null;
    document.body.style.userSelect = '';

    if (moved)
        setRouting(routingFromDOM());
}

//
// Flicking a card out of the chain.
//
// The eject button does this too, and did it first.  This exists because
// a card that can be dragged around looks like a thing you can move, and
// throwing something you are holding off to one side to be rid of it is
// what a hand reaches for before it reaches for a button.
//
// It only had room to exist once a finger stopped starting drags from
// the whole header.  Vertical there scrolls the page and sideways is
// ours, which is what 'touch-action: pan-y' on the header says, and the
// page has nowhere to go sideways so claiming it costs nothing.
//
// Touch only.  A mouse still grabs the header to reorder, and the same
// gesture cannot also mean discard.
//
const SWIPE_ARM = 12;           // px sideways before this is a swipe at all
const SWIPE_COMMIT = 70;        // px, or a quarter of the card, to let go
const SWIPE_RETURN_MS = 180;    // spring-back, and .swipe-return in the css

let cardSwipe = null;

// How far it has to go before letting go means it. A quarter of the card
// on a wide screen, and a thumb's worth on a narrow one.
function swipeCommit(card) {
    return Math.max(SWIPE_COMMIT, card.offsetWidth / 4);
}

function cardSwipeStart(card, e) {
    if (!e.isPrimary || cardSwipe || cardDrag)
        return;

    // Reordering is the mouse's gesture on this header - see cardDragStart()
    if (e.pointerType === 'mouse')
        return;

    // The handle reorders, and the header's own controls keep their presses
    if (e.target.closest('.drag-handle, .collapse-chevron, .action-btn'))
        return;

    cardSwipe = { card, id: e.pointerId,
                  x: e.clientX, y: e.clientY, armed: false };

    window.addEventListener('pointermove', cardSwipeMove);
    window.addEventListener('pointerup', cardSwipeEnd);
    window.addEventListener('pointercancel', cardSwipeRelease);
}

function cardSwipeMove(e) {
    if (!cardSwipe || e.pointerId !== cardSwipe.id)
        return;

    const dx = e.clientX - cardSwipe.x;
    const dy = e.clientY - cardSwipe.y;

    if (!cardSwipe.armed) {
        //
        // Sideways, and more sideways than not.  The second half is what
        // keeps a scroll that starts with a wobble from arming this: the
        // browser is about to take the gesture for panning, and it says
        // so with a pointercancel, but it is worth not having moved the
        // card in the meantime.
        //
        if (Math.abs(dx) < SWIPE_ARM || Math.abs(dx) < Math.abs(dy))
            return;

        cardSwipe.armed = true;
        cardSwipe.card.classList.remove('swipe-return');
    }

    //
    // Fades as it goes, so how far is left to go is visible without
    // anything having to be drawn behind it.
    //
    const gone = Math.min(1, Math.abs(dx) / swipeCommit(cardSwipe.card));
    cardSwipe.card.style.transform = `translateX(${dx}px)`;
    cardSwipe.card.style.opacity = 1 - 0.6 * gone;
}

//
// Put the card back where it belongs, whatever happens next.
//
// Cards are reused rather than rebuilt, so one let go halfway would
// otherwise still be sitting at that offset when it is routed again.
//
function cardSwipeRelease() {
    const swipe = cardSwipe;

    cardSwipe = null;
    window.removeEventListener('pointermove', cardSwipeMove);
    window.removeEventListener('pointerup', cardSwipeEnd);
    window.removeEventListener('pointercancel', cardSwipeRelease);

    if (!swipe || !swipe.armed)
        return swipe;

    //
    // Taken off again once it has landed.  The class names a transition,
    // and .effect-card already has one it cares about - the attention
    // glow, which is deliberately instant in one direction - so this is
    // not something to leave sitting on a card afterwards.
    //
    swipe.card.classList.add('swipe-return');
    swipe.card.style.transform = '';
    swipe.card.style.opacity = '';
    setTimeout(() => swipe.card.classList.remove('swipe-return'),
               SWIPE_RETURN_MS);
    return swipe;
}

function cardSwipeEnd(e) {
    const swipe = cardSwipeRelease();

    if (!swipe || e.pointerId !== swipe.id || !swipe.armed)
        return;

    // Not far enough: it has already sprung back
    if (Math.abs(e.clientX - swipe.x) < swipeCommit(swipe.card))
        return;

    //
    // Unrouting parks the card, so the spring-back the line above set
    // going never gets a frame to be seen in - the card is gone before
    // it can travel. Which is the intent: it went the way it was thrown.
    //
    unrouteEffect(parseInt(swipe.card.dataset.effectId));
}

//
// A tap, as opposed to a touch.
//
// These are two different things and the difference is the whole bug:
// this used to open the panel from 'touchstart', which is the moment a
// finger lands and before anybody - the browser included - knows what
// the gesture is going to be.  So scrolling the page with a finger that
// happened to start on a pot threw a modal up over what you were
// scrolling towards, every time.
//
// A tap is a press that never goes anywhere.  That can only be known at
// the end, so the decision is made on pointerup, and a gesture the
// browser takes over for scrolling comes back as pointercancel, which is
// exactly the answer we want: not a tap.
//
// "Never went anywhere" has to be remembered rather than measured at the
// end.  Comparing where the finger landed against where it left is not
// the same question, and gets the interesting case backwards: drag a
// slider up and back down and you release within a few px of where you
// started, having very much moved.  That is not a hypothetical - it is
// the ordinary way to use a slider, and it put the panel up on top of
// the value you had just finished setting.
//
// So the flag is sticky, exactly like cardDrag.moved next door: once
// this gesture has moved, it is not a tap again.
//
const TAP_SLOP = 10;    // px of travel a tap is allowed

let potTap = null;

function releasePotTap() {
    potTap = null;
    window.removeEventListener('pointermove', movePotTap);
    window.removeEventListener('pointerup', endPotTap);
    window.removeEventListener('pointercancel', releasePotTap);
}

function movePotTap(e) {
    if (!potTap || e.pointerId !== potTap.id)
        return;

    if (Math.abs(e.clientX - potTap.x) > TAP_SLOP ||
        Math.abs(e.clientY - potTap.y) > TAP_SLOP)
        potTap.moved = true;
}

function endPotTap(e) {
    const tap = potTap;

    releasePotTap();
    if (!tap || e.pointerId !== tap.id)
        return;

    // It travelled: a scroll, or a drag of the control itself
    if (tap.moved)
        return;

    tap.open();
}

//
// Open something when this element is tapped, without stealing a scroll
// that happens to begin on it.
//
// 'grab' is the part of it that a mouse can already operate directly -
// the inline slider - and a mouse press there is a drag of it rather
// than a request to open anything.  A mouse has no scroll gesture to be
// confused with, so it does not wait for the release: anywhere else on
// the control opens immediately, which is how it always behaved.
//
function openOnTap(el, grab, open) {
    el.addEventListener('pointerdown', (e) => {
        if (!e.isPrimary)
            return;

        if (e.pointerType === 'mouse') {
            if (e.target !== grab)
                open();
            return;
        }

        releasePotTap();
        potTap = { id: e.pointerId, x: e.clientX, y: e.clientY,
                   moved: false, open };
        window.addEventListener('pointermove', movePotTap);
        window.addEventListener('pointerup', endPotTap);
        window.addEventListener('pointercancel', releasePotTap);
    });
}

//
// The chain, as an ordered list of effect ids.  Kept up to date by
// applyRouting(), which is the one place routing becomes known - whether
// we decided it or the pedal told us.
//
// This is the model, and the cards are drawn from it.  It used to be the
// other way around: routing was read back out of the dom by walking the
// cards and stopping at a divider, which made card position the truth and
// left routing, unrouting and reordering as three meanings for one
// gesture.  That is what made the pool a long sortable list of things you
// were not doing anything with, and it is why the two anchor effects had
// to be recognised by name everywhere the list was walked.
//
let currentRouting = [];

// The firmware routes at most this many; the rest are silently dropped
const MAX_ROUTED = 14;

// Effect id -> its card, filled in by renderUI()
const effectCards = new Map();

// The unrouted pool, which renderUI() creates and applyRouting() fills
let effectPool = null;

//
// The two effects that are not in the routing order at all: the signal
// chain at the front, and the settings pseudo-effect at the back.
//
// Positional, because that is the firmware's own test - ROUTABLE_EFFECTS
// is the bits *between* the first entry and the last - and because
// anything else is a string that has to keep agreeing across two
// languages.  A pedal older than a rename now puts its anchors in the
// right place and merely shows a stale label, which is honest: a stale
// label is what is running.
//
function isAnchorEffect(idx) {
    return idx === 0 || idx === PEDAL_EFFECTS.length - 1;
}

//
// What the pedal says it is doing.  See MIDI_CC_MAP.md for the wire
// format; effects.js has the numbers, taken from the firmware's midi.h.
//
// Attention is kept as the raw bits rather than applied as they arrive,
// because two CCs contribute to one picture: the chain pair covers the
// routed effects by position, and one bit of the global CC covers the
// Signal Chain, which is not in the chain to have a position. Re-render
// from both whenever either moves, or whenever the routing does.
//
let statusChainBits = 0;
let statusFrontAttn = false;
let pedalIdentity = null;

//
// Faults are shown for as long as the pedal says so - but never for less
// than this.
//
// The floor is on how long it stays up, not on how long we believe it,
// and the difference is the whole thing.  The pedal reports state, on
// change and then periodically; a condition that persists is one message
// and then the same message again.  A timer that expired would hide a
// clip that is still happening and then wait forever for a change that
// has no reason to come - which is exactly what it did.
//
// The floor exists because the other extreme is just as invisible: one
// clipped sample sets the bit for a single 40ms tick and clears it again,
// and nobody sees 40ms.
//
const STATUS_MIN_MS = 900;

const clipFault = { id: 'status-clip', on: false, since: 0, timer: null };
const dropFault = { id: 'status-drop', on: false, since: 0, timer: null };

// Neither of these is a transient: both come from the identity reply and
// are never timed out. boardFault means broken; earlyNote means old.
const boardFault = { id: 'status-board', on: false, since: 0, timer: null };
const earlyNote = { id: 'status-early', on: false, since: 0, timer: null };

const allFaults = [clipFault, dropFault, boardFault, earlyNote];

function paintFaults() {
    for (const f of allFaults)
        document.getElementById(f.id).classList.toggle('hidden', !f.on);

    // The wrapper goes with them, so an idle header has nothing in it
    document.getElementById('pedal-status')
            .classList.toggle('hidden', !allFaults.some(f => f.on));
}

function setFault(f, on) {
    if (on) {
        // Still happening: cancel any pending hide and restart the floor
        clearTimeout(f.timer);
        f.timer = null;
        f.on = true;
        f.since = Date.now();
        paintFaults();
        return;
    }

    if (!f.on || f.timer)
        return;

    const left = STATUS_MIN_MS - (Date.now() - f.since);
    if (left <= 0) {
        f.on = false;
        paintFaults();
    } else {
        f.timer = setTimeout(() => {
            f.timer = null;
            f.on = false;
            paintFaults();
        }, left);
    }
}

function renderAttention() {
    const lit = new Set();

    if (statusFrontAttn) {
        const front = PEDAL_EFFECTS.find(e => e.base === 'signal_chain');
        if (front) lit.add(front.id);
    }
    currentRouting.forEach((effectId, pos) => {
        if ((statusChainBits >> pos) & 1) lit.add(effectId);
    });

    // Every card, not just the lit ones - an effect that stops asking,
    // or that leaves the chain while lit, has to go dark again.
    PEDAL_EFFECTS.forEach((effect, idx) => {
        const card = document.getElementById(`effect-${idx}`);
        if (card) card.classList.toggle('attention', lit.has(effect.id));
    });
}

//
// Forget everything the pedal told us about itself.  A pedal that goes
// away stops reporting rather than reporting that it is fine, so its last
// word stays on the screen until somebody clears it.
//
function clearPedalStatus() {
    for (const f of allFaults) {
        clearTimeout(f.timer);
        f.timer = null;
        f.on = false;
    }
    document.getElementById('identity-info').textContent = 'Not connected.';
    paintFaults();

    statusChainBits = 0;
    statusFrontAttn = false;
    renderAttention();
}

//
// The menu says which scene it is about to overwrite, so it has to be
// refreshed whenever the selection moves - including when the picker is
// rebuilt from the identity reply, which is why this is not tucked inside
// the setup that binds it.
//
function updateSceneLabels() {
    const sel = document.getElementById('global-scene-select');
    const scene = sel ? sel.value : '0';
    const saveBtn = document.getElementById('global-save-scene-btn');
    const loadBtn = document.getElementById('global-load-scene-btn');
    if (saveBtn) saveBtn.innerHTML = `💾&nbsp;&nbsp;Save to Scene ${scene}`;
    if (loadBtn) loadBtn.innerHTML = `📂&nbsp;&nbsp;Load Scene ${scene}`;
}

//
// The scene picker, sized by whatever the pedal reports.
//
// Rebuilt rather than adjusted, because reconnecting to a different pedal
// can change the count, and a stale option would offer a scene that
// cannot be loaded.
//
function populateScenePicker(count) {
    const sel = document.getElementById('global-scene-select');
    if (!sel)
        return;

    const was = sel.value;
    sel.innerHTML = '';
    for (let i = 0; i < count; i++) {
        const opt = document.createElement('option');
        opt.value = i;
        opt.textContent = `Scene ${i}`;
        sel.appendChild(opt);
    }
    if (was !== '' && Number(was) < count)
        sel.value = was;
    updateSceneLabels();
}

//
// Telemetry, polled while somebody is watching.
//
// The layout is append-only, so read the fields this app knows about and
// stop.  A frame shorter than expected is firmware from before the tail
// existed; a longer one is firmware from after this app was written.
// Neither is an error, and neither means zero - so every field is fetched
// through a helper that can answer "absent", and an absent field is left
// out of the readout rather than shown as a dash.
//
const TELEMETRY_POLL_MS = 200;
let telemetryTimer = null;

// 127 is the saturation the pedal sends for silence
function formatDbfs(byte) {
    return byte >= 127 ? '\u2212\u221e' : `\u2212${byte}`;
}

function handleTelemetry(data) {
    // The whole message: F0 7D 0B <version> <fields...> F7
    const at = (i) => (3 + i < data.length - 1 ? data[3 + i] : undefined);

    if (at(0) === undefined)
        return;

    const inPeak = at(1), floor = at(2), out = at(3);
    const gate = at(4), loadHi = at(5), loadLo = at(6);
    const bits = [];

    if (inPeak !== undefined) bits.push(`in ${formatDbfs(inPeak)} dB`);
    if (floor !== undefined) bits.push(`floor ${formatDbfs(floor)} dB`);
    if (out !== undefined) bits.push(`out ${formatDbfs(out)} dB`);

    //
    // The gate as how far it is holding the signal down, not as a
    // percentage of a multiplier: "\u221218 dB" is the number you compare
    // against Level, which is also in dB.
    //
    if (gate !== undefined) {
        if (gate >= 127)
            bits.push('gate open');
        else if (gate === 0)
            bits.push('gate closed');
        else
            bits.push(`gate \u2212${Math.round(-20 * Math.log10(gate / 127))} dB`);
    }
    //
    // The audio core's share of the sample period, which is the one
    // number here with a hard ceiling: at 100% there is no idle left and
    // the pedal starts dropping samples.
    //
    // Fourteen bits from layout 2 - an MSB where the seven-bit value
    // used to be, an LSB after it - and taken by presence rather than by
    // version, which is how every field on this line is read.  Older
    // firmware simply has no LSB and gets the coarse reading.
    //
    // The decimal place is worth having and was not before.  One
    // seven-bit step is 0.8% of the sample period while a whole reverb
    // is about 22%, so rounding to a percent threw away a tenth of the
    // most expensive effect the pedal has - and the headroom left is
    // exactly what somebody stacking effects is looking at this for.
    //
    if (loadHi !== undefined) {
        const load = loadLo === undefined
                   ? loadHi / 127
                   : ((loadHi << 7) | loadLo) / 16383;
        bits.push(`cpu ${(load * 100).toFixed(1)}%`);
    }

    const el = document.getElementById('signal-chain-meters');
    if (el) el.textContent = bits.join('  \u00b7  ');
}

//
// Only while there is a pedal and somebody is looking at the page.  A
// backgrounded tab polling five times a second is MIDI traffic spent on
// meters nobody can see.
//
function updateTelemetryPolling() {
    const want = !!midiOutput && document.visibilityState === 'visible';

    if (want && !telemetryTimer) {
        telemetryTimer = setInterval(() => sendSysex([SYSEX_CMD.TELEMETRY]),
                                     TELEMETRY_POLL_MS);
    } else if (!want && telemetryTimer) {
        clearInterval(telemetryTimer);
        telemetryTimer = null;
        const el = document.getElementById('signal-chain-meters');
        if (el) el.textContent = 'no readings yet';
    }
}

document.addEventListener('visibilitychange', updateTelemetryPolling);

//
// What the pedal says it is.  Asked once, on connect.
//
// The build stamp is the useful part day to day: it answers "is this
// running what I just built", which is a question that has wasted more
// than one evening - a schema from stale firmware renders perfectly and
// is simply about a different pedal.
//
// The probe results are the rare-but-serious part.  A TAC5112 or an
// SH1106 answering means the board predates this firmware by a couple of
// generations and nothing is going to work properly, so that gets a chip
// in the header rather than a line in a dialog nobody has open.
//
function handleIdentity(id) {
    pedalIdentity = id;
    CONTROLS = id.controls || [];
    renderBindings();

    const found = id.found || {};
    const notes = [`Firmware built ${id.build || 'unknown'}.`];
    const wrong = [];
    const early = [];
    const scenes = id.scenes || 1;

    //
    // How many scenes there are is the pedal's to say, not ours to
    // assume. A 2kbit eeprom holds one; the 64kbit part holds 32.
    //
    populateScenePicker(scenes);

    notes.push(`${scenes} scene${scenes === 1 ? '' : 's'}` +
               `, hardware MIDI ${id.midi_hw ? 'on' : 'off'}.`);

    //
    // An early board is not a fault. Those play: the pedal sets the
    // TAC5112 up over i2c, and the eeprom geometry is worked out rather
    // than compiled in, so one scene on a 2kbit part saves and loads.
    // Mono, because they never routed the second channel. Worth knowing
    // which board is on the bench, not worth alarming anybody.
    //
    if (found.legacy_codec)
        early.push('A TAC5112 codec answered on i2c, so this is an early ' +
                   'board: mono only, and the codec is set up over i2c ' +
                   'rather than strapped.');
    if (found.legacy_screen)
        early.push('An SH1106 screen answered on i2c, from a generation ' +
                   'that had one. Nothing drives it.');

    boardFault.on = wrong.length > 0;
    earlyNote.on = early.length > 0;
    document.getElementById('status-board').title = wrong.join(' ');
    document.getElementById('status-early').title = early.join(' ');
    paintFaults();

    document.getElementById('identity-info').textContent =
        notes.concat(wrong, early).join(' ');
}

//
// Bench tool, on window so it can be driven from the console.
//
window.dumpEeprom = function (blk = 0, count = 1) {
    for (let i = 0; i < count; i++)
        setTimeout(() => sendSysex([0x0e, blk + i]), i * 50);
};


//
// Send the whole table.  Nothing optimistic happens here: the pedal
// drops rules it does not like and echoes back what it kept, and that
// is what redraws.  So the screen shows what the pedal has rather than
// what it was asked for, and a rule that did not survive is visible as
// a row that did not come back.
//
function sendRules(list, level = RULES_SCENE) {
    const out = [SYSEX_SET_BINDING, level];
    list.forEach(r => out.push(r.control & 0x7f, r.action & 0x7f,
                               r.effect & 0x7f, (r.pot || 0) & 0x7f,
                               (r.val[0] || 0) & 0x7f,
                               (r.val[1] || 0) & 0x7f));
    sendSysex(out);
}

// Replace one rule, or drop it when 'r' is null.
//
// Make this control the scene's before changing it.
//
// Resolution is per control and the most specific level wins outright,
// so the moment a scene says anything about a control it says
// everything.  A scene that inherits four rules and wants to change one
// therefore has to take all four, or the other three vanish.
//
function promoteControl(c) {
    if (pedalRules.some(r => r.control === c))
        return pedalRules;
    return pedalRules.concat(
        rulesByLevel[RULES_EFFECTIVE].filter(r => r.control === c));
}

function putRule(i, r) {
    if (i < 0) {
        // An inherited row: take the control first, then edit the copy.
        const list = promoteControl(r.control);
        const at = list.findIndex(x => x.control === r.control);
        if (r)
            list[at] = r;
        sendRules(list);
        return;
    }
    const list = pedalRules.slice();
    if (r)
        list[i] = r;
    else
        list.splice(i, 1);
    sendRules(list);
}

function potDefFor(effId, potIdx) {
    if (potIdx === 0)
        return MIX_POT_DEF;
    const e = PEDAL_EFFECTS[effectIdMap.get(effId)];
    return e && e.pots ? e.pots[potIdx - 1] : null;
}

function potLabel(effId, potIdx) {
    const pot = potDefFor(effId, potIdx);
    const e = PEDAL_EFFECTS[effectIdMap.get(effId)];

    if (!e || !pot || !pot.name)
        return `effect ${effId}, parameter ${potIdx}`;
    return `${e.name} \u2013 ${pot.name}`;
}

//
// Every (effect, pot) worth pointing something at, in the order the
// signal travels: the front anchor, the routed chain, the back anchor.
//
// An unrouted effect is left out because pointing at one would be
// pointing at nothing - the pedal does not step an effect that is not
// in the chain, and unrouting one throws its values away, so the value
// being set is about to be overwritten by a default.  The two anchors
// are always in even though neither is in effect_chain[]: the Signal
// Chain always runs, and Settings is where the attention brightness
// lives, which is the one setting whose whole point is to be adjusted
// from the pedal while watching the LED.
//
function potTargets() {
    const out = [];
    const seen = new Set();

    const add = (id) => {
        const idx = effectIdMap.get(id);
        const e = PEDAL_EFFECTS[idx];
        if (!e || seen.has(id))
            return;
        seen.add(id);
        //
        // The mix first, because turning an effect off by taking its
        // mix to nothing is the most useful thing a footswitch can do
        // to one.  The anchors have no wet and no dry, so no mix.
        //
        if (!isAnchorEffect(idx))
            out.push({ effId: e.id, pot: 0, label: potLabel(e.id, 0) });
        e.pots.forEach((pot, i) => {
            if (pot.name)
                out.push({ effId: e.id, pot: i + 1,
                           label: potLabel(e.id, i + 1) });
        });
    };

    if (PEDAL_EFFECTS.length)
        add(PEDAL_EFFECTS[0].id);
    currentRouting.forEach(add);
    if (PEDAL_EFFECTS.length > 1)
        add(PEDAL_EFFECTS[PEDAL_EFFECTS.length - 1].id);

    return out;
}

function sceneCount() {
    const sel = document.getElementById('global-scene-select');
    return sel && sel.options.length ? sel.options.length : 1;
}

function bindingSummary(b) {
    const def = actionDef(b.action);

    if (b.action === ACT.SCENE)
        return `Scene ${b.effect}`;
    if (!def.target)
        return def.label;
    if (b.effect === BIND_FOLLOW)
        return `${def.label}: the knob\u2019s`;
    return `${def.label}: ${potLabel(b.effect, b.pot)}`;
}

//
// One number, drawn the way that pot's own control is drawn: a menu for
// something enumerated, a slider and a readout for anything else.
//
// It has to be the pot's own units.  A raw 0-120 would make "toggle
// between 40 and 80" unanswerable without doing the arithmetic that the
// rest of the app exists to avoid.
//
function valueControl(pot, val, onChange) {
    const wrap = document.createElement('div');
    wrap.className = 'binding-value';

    if (pot && pot.curve === 'ENUM' && pot.enum && pot.enum.length) {
        const sel = document.createElement('select');
        sel.className = 'menu-select';
        pot.enum.forEach((name, i) => {
            const o = document.createElement('option');
            o.value = i;
            o.textContent = name;
            sel.appendChild(o);
        });
        sel.value = Math.min(val, pot.enum.length - 1);
        sel.addEventListener('change', () => onChange(parseInt(sel.value)));
        wrap.appendChild(sel);
        return wrap;
    }

    const range = document.createElement('input');
    range.type = 'range';
    range.min = 0;
    range.max = 120;
    range.value = val;

    const out = document.createElement('span');
    out.className = 'pot-value';
    const show = (v) => out.textContent = pot ? formatPotValue(pot, v) : v;
    show(val);

    // Follow the drag, but only send when it is let go of.
    range.addEventListener('input', () => show(parseInt(range.value)));
    range.addEventListener('change', () => onChange(parseInt(range.value)));

    wrap.appendChild(range);
    wrap.appendChild(out);
    return wrap;
}

//
// A sensible rule to start from when a gesture gains one.
//
// A click gets a mix toggle on the first thing in the chain, because
// that is overwhelmingly what a footswitch is added for, and an empty
// row you then have to fill in twice is not a starting point.
//
function newRule(ctrl) {
    const t = potTargets()[0];

    if (controlDef(ctrl).kind === 'turn')
        return { control: ctrl, action: ACT.POT,
                 effect: t ? t.effId : 0, pot: t ? t.pot : 0, val: [0, 0] };
    return { control: ctrl, action: ACT.TOGGLE_POT,
             effect: t ? t.effId : 0, pot: t ? t.pot : 0, val: [0, 120] };
}

function renderRule(r, i) {
    const def = actionDef(r.action);
    const row = document.createElement('div');
    row.className = 'rule-row';

    //
    // Everything the rule says goes in the body, which wraps onto a
    // second line when there are values.  The remove button sits beside
    // the whole thing rather than in the flow, because it belongs to
    // the rule and not to any one line of it.
    //
    const body = document.createElement('div');
    body.className = 'rule-body';
    if (r.action === ACT.SCENE || def.target)
        body.classList.add('has-target');
    row.appendChild(body);

    const act = document.createElement('select');
    act.className = 'menu-select';
    actionsFor(r.control).forEach(a => {
        const o = document.createElement('option');
        o.value = a.v;
        o.textContent = a.label;
        act.appendChild(o);
    });
    act.value = r.action;
    body.appendChild(act);

    act.addEventListener('change', () => {
        const a = parseInt(act.value);
        const nd = actionDef(a);
        const next = { control: r.control, action: a,
                       effect: 0, pot: 0, val: [0, 0] };

        //
        // A new action inherits the target when it can still use one,
        // because changing "reset this" to "toggle this" and being
        // handed a different parameter would be a surprise.  What it
        // cannot inherit is a following target, which only reset may
        // have.
        //
        if (a === ACT.SCENE) {
            next.effect = (r.action === ACT.SCENE) ? r.effect : 0;
        } else if (nd.target) {
            const usable = r.effect !== BIND_FOLLOW || nd.follow;
            if (def.target && usable) {
                next.effect = r.effect;
                next.pot = r.pot;
            } else if (nd.follow) {
                next.effect = BIND_FOLLOW;
            } else {
                const t = potTargets()[0];
                if (t) { next.effect = t.effId; next.pot = t.pot; }
            }
            if (nd.values && next.effect !== BIND_FOLLOW)
                next.val = [r.val[0], r.val[1]];
        }
        putRule(i, next);
    });

    if (r.action === ACT.SCENE || def.target) {
        const arg = document.createElement('select');
        arg.className = 'menu-select';

        if (r.action === ACT.SCENE) {
            for (let sc = 0; sc < sceneCount(); sc++) {
                const o = document.createElement('option');
                o.value = `s${sc}`;
                o.textContent = `Scene ${sc}`;
                arg.appendChild(o);
            }
            arg.value = `s${r.effect}`;
        } else {
            if (def.follow) {
                const o = document.createElement('option');
                o.value = 'follow';
                o.textContent = 'The knob\u2019s parameter';
                arg.appendChild(o);
            }
            const targets = potTargets();
            const cur = `${r.effect}:${r.pot}`;
            //
            // Whatever it points at stays in the list even after that
            // effect leaves the chain.  Dropping it would make the
            // select show some other parameter's name while the pedal
            // went on driving this one.
            //
            if (r.effect !== BIND_FOLLOW &&
                !targets.some(t => `${t.effId}:${t.pot}` === cur))
                targets.unshift({ effId: r.effect, pot: r.pot,
                                  label: potLabel(r.effect, r.pot) +
                                         ' (not routed)' });
            targets.forEach(t => {
                const o = document.createElement('option');
                o.value = `${t.effId}:${t.pot}`;
                o.textContent = t.label;
                arg.appendChild(o);
            });
            arg.value = r.effect === BIND_FOLLOW ? 'follow' : cur;
        }

        arg.addEventListener('change', () => {
            const v = arg.value;
            const next = { ...r, val: r.val.slice() };
            if (v.startsWith('s'))
                next.effect = parseInt(v.slice(1));
            else if (v === 'follow') {
                next.effect = BIND_FOLLOW;
                next.pot = 0;
            } else {
                const [e, p] = v.split(':').map(Number);
                next.effect = e;
                next.pot = p;
            }
            putRule(i, next);
        });
        body.appendChild(arg);
    }

    if (def.values && r.effect !== BIND_FOLLOW) {
        const pot = potDefFor(r.effect, r.pot);
        for (let v = 0; v < def.values; v++) {
            body.appendChild(valueControl(pot, r.val[v], (nv) => {
                const next = { ...r, val: r.val.slice() };
                next.val[v] = nv;
                putRule(i, next);
            }));
        }
    }

    const del = document.createElement('button');
    del.className = 'action-btn rule-remove';
    del.textContent = '\u00d7';
    del.title = 'Remove';
    del.addEventListener('click', () => putRule(i, null));
    row.appendChild(del);

    return row;
}

function renderBindings() {
    const host = document.getElementById('bindings-rows');
    const hint = document.getElementById('bindings-hint');

    if (hint)
        hint.textContent = haveRules
            ? 'What each control does now. Rules belong to the scene and ' +
              'are kept when you save it; a control this scene says ' +
              'nothing about is shown greyed, inherited from the ' +
              'pedal-wide rules or from the built-in defaults. Editing ' +
              'one makes it this scene\u0027s. A gesture can have more ' +
              'than one rule and they all happen together \u2014 which is ' +
              'how one press swaps between two effects.'
            : 'Not connected.';

    if (!host)
        return;
    host.innerHTML = '';

    //
    // Rows only once the pedal has told us what it has.  An empty table
    // here would claim every control does nothing, which is a specific
    // and wrong answer to a question we have not asked yet.
    //
    if (!haveRules || !PEDAL_EFFECTS.length || !CONTROLS.length)
        return;

    CONTROLS.forEach(ctrl => {
        const c = ctrl.id;

        const group = document.createElement('div');
        group.className = 'rule-group';

        //
        // The gesture's name, and adding to it, on one line - so that
        // the button lands in the same column as the removes below it
        // and it is obvious which gesture it adds to.
        //
        const head = document.createElement('div');
        head.className = 'rule-head';

        const label = document.createElement('label');
        label.textContent = ctrl.name;
        head.appendChild(label);

        const add = document.createElement('button');
        add.className = 'action-btn rule-add';
        add.textContent = '+';
        add.title = `Add a rule to ${ctrl.name}`;
        add.disabled = pedalRules.length >= 16;
        add.addEventListener('click', () =>
            sendRules(promoteControl(c).concat([newRule(c)])));
        head.appendChild(add);
        group.appendChild(head);

        //
        // The effective table, not the scene's.  A scene that says
        // nothing about a control still has that control doing
        // something - the pedal-wide rules or the built-in defaults -
        // and drawing an empty row would claim it does nothing, which
        // is a specific and wrong answer.
        //
        // Inherited rows carry no index into the scene's list because
        // they are not in it.  editing one promotes it first; see
        // promoteControl().
        //
        const own = pedalRules.filter(r => r.control === c);
        const shown = own.length ? own
                                 : rulesByLevel[RULES_EFFECTIVE]
                                       .filter(r => r.control === c);

        shown.forEach(r => {
            const i = pedalRules.indexOf(r);
            const row = renderRule(r, i);
            if (i < 0)
                row.classList.add('inherited');
            group.appendChild(row);
        });

        host.appendChild(group);
    });
}

//
// What the knob is on, said next to the pot you are looking at.
//
// Without this the shortcut below is a button that silently steals the
// knob from wherever it was - and with one knob, every assignment is a
// theft from something.
//
function renderKnobHint() {
    const hintEl = document.getElementById('knob-target-hint');
    const btn = document.getElementById('assign-knob-btn');
    if (!hintEl || !btn)
        return;

    const b = pedalRules.find(r => r.control === 0 && r.action === ACT.POT);
    const here = activePotTarget();

    if (here && b && b.effect === here.effId && b.pot === here.pot) {
        hintEl.textContent = 'The knob is on this.';
        btn.disabled = true;
    } else {
        hintEl.textContent = b
            ? `Knob is on ${potLabel(b.effect, b.pot)}.`
            : 'The knob drives nothing.';
        btn.disabled = !here;
    }
}

// Which (effect, parameter) the open panel is showing, mix included.
function activePotTarget() {
    if (!activePotCc)
        return null;

    const parts = activePotCc.split('-');
    const eff = PEDAL_EFFECTS[parseInt(parts[1])];
    if (!eff)
        return null;

    if (parts.length >= 4 && parts[2] === 'pot')
        return { effId: eff.id, pot: parseInt(parts[3]) + 1 };
    if (parts.length === 3 && parts[2] === 'mix')
        return { effId: eff.id, pot: 0 };
    return null;
}

function handleGlobalStatus(val) {
    const dropped = val & STATUS_DROPPED_MASK;

    statusFrontAttn = (val & STATUS_FRONT_ATTN) !== 0;
    renderAttention();

    setFault(clipFault, (val & STATUS_CLIPPED) !== 0);

    // Only relabel while there is something to say, so the count doesn't
    // change under a reading that is still being held up
    if (dropped) {
        // Saturated, so say so rather than claiming it was exactly 31
        document.getElementById(dropFault.id).textContent =
            dropped === STATUS_DROPPED_MASK ? `DROP ${dropped}+`
                                            : (dropped > 1 ? `DROP ${dropped}` : 'DROP');
    }
    setFault(dropFault, dropped !== 0);
}

//
// An effect that isn't routed has no values: it goes back to the
// defaults from the schema.
//
// We do that here, in the app, rather than letting the pedal reset and
// then asking it what happened.  Going and fetching the state back is a
// round trip we would have to race against, and the answer arrives in a
// couple of hundred SysEx messages - so the sliders end up showing the
// old values for a while, or a mix of old and new.  Doing it locally and
// pushing the result means the UI is right by construction.
//
// Only effects that just *left* the chain are touched.  Rewriting every
// unrouted effect on every reorder would be a flood for no reason.
//
function resetUnroutedEffects(newRouted) {
    const wasRouted = new Set(currentRouting);
    const nowRouted = new Set(newRouted);

    PEDAL_EFFECTS.forEach((effect, idx) => {
        if (!wasRouted.has(effect.id) || nowRouted.has(effect.id)) return;

        effect.pots.forEach((potDef, pIdx) => {
            const val = getInitialPotValue(potDef);
            const el = ccToElementMap.get(`eff-${idx}-pot-${pIdx}`);
            if (el) {
                el.value = val;
                const disp = el.parentElement.querySelector('.pot-value');
                if (disp) disp.textContent = formatPotValue(potDef, val);
                if (el.redrawCurve) el.redrawCurve();
            }
            sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, pIdx + 1, val]);
        });

        const mixVal = Math.round((effect.defMix !== undefined ? effect.defMix : 1.0) * 120);
        const mixEl = ccToElementMap.get(`eff-${idx}-mix`);
        if (mixEl) {
            mixEl.value = mixVal;
            const disp = mixEl.parentElement.querySelector('.pot-value');
            if (disp && mixEl.potDef) disp.textContent = formatPotValue(mixEl.potDef, mixVal);
        }
        sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, 0, mixVal]);
    });
}

//
// A routing change: tell the pedal, then draw what we asked for.
//
// Everything that changes the chain goes through here, so there is one
// place that sends ROUTING_ORDER and one place that decides what the
// cards look like afterwards.
//
function setRouting(ids) {
    // Cap before the list is used for anything.  Drawing the uncapped
    // list would show effects in the chain that the pedal never heard
    // about, since it drops the overflow without comment.
    const routed = ids.slice(0, MAX_ROUTED);

    sendSysex([SYSEX_CMD.ROUTING_ORDER, ...routed]);
    // Before applyRouting(), which is what moves currentRouting on
    resetUnroutedEffects(routed);
    applyRouting(routed);
}

//
// Onto the end of the chain, which is where a new effect goes - and then
// go and look at it, because the end of the chain is somewhere else.
//
// Tapping a chip is asking for that effect, and what you get back is a
// card you cannot see: the pool is below the whole chain, the card lands
// at the far end of it, and everything shifts as the pool shrinks.  So
// the one thing you asked for is the one thing not on the screen.
//
// Centred rather than just scrolled into view, because "just far enough"
// puts it hard against an edge with its controls half off - and the
// movement is worth having in its own right. It is what says where the
// effect went, which is a thing about the chain worth knowing.
//
function routeEffect(id) {
    if (currentRouting.includes(id) || currentRouting.length >= MAX_ROUTED)
        return;

    setRouting([...currentRouting, id]);
    showEffectCard(id);
}

function showEffectCard(id) {
    const card = effectCards.get(id);
    if (!card)
        return;

    // Somebody who has asked not to be moved about gets put there
    // directly instead
    const still = window.matchMedia &&
                  window.matchMedia('(prefers-reduced-motion: reduce)').matches;

    card.scrollIntoView({ block: 'center',
                          behavior: still ? 'auto' : 'smooth' });
}

function unrouteEffect(id) {
    if (!currentRouting.includes(id))
        return;
    setRouting(currentRouting.filter(other => other !== id));
}

//
// After a drag, the cards are the order.
//
// The one place the dom is read rather than written, and it only reads
// the order of what is already in the chain: whether an effect is routed
// at all is decided by the buttons, never by where a card came to rest.
//
function routingFromDOM() {
    const ids = [];
    for (const el of effectsContainer.children) {
        if (isRoutedCard(el))
            ids.push(parseInt(el.dataset.effectId));
    }
    return ids;
}


function getInitialPotValue(pot) {
    // The generator hands us the exact raw value the firmware has in its
    // own table, so there is nothing to recompute and nothing to get
    // subtly wrong. The rest of this is only a fallback for a pedal
    // running a schema older than that field.
    if (pot.defaultPot !== undefined) return pot.defaultPot;

    if (pot.default === undefined) return 60;
    return valueToPot(pot, pot.default);
}


//
// Collapsing a card.
//
// Expanding clears the inline display rather than setting one, so the
// stylesheet decides what the pot layout is.  Naming a value here meant
// that a card which had been collapsed once came back as a flex row
// instead of the grid it started as.
//
function setCardCollapsed(card, collapsed) {
    const controls = card.querySelector('.effect-controls');
    const chevron = card.querySelector('.collapse-chevron');

    if (controls) controls.style.display = collapsed ? 'none' : '';
    if (chevron) chevron.style.transform = collapsed ? 'rotate(-90deg)' : 'rotate(0deg)';
}

//
// Draw the list from the routing.
//
// Cards for unrouted effects are parked rather than removed: the pedal
// still addresses their pots by id, ccToElementMap points straight at the
// inputs inside them, and rebuilding one on every routing change would
// invalidate both. They are hidden, and the pool speaks for them.
//
function applyRouting(routeIds) {
    const wasRouted = new Set(currentRouting);
    currentRouting = routeIds.slice();

    // What the knob can be pointed at is a function of what is in the
    // chain, so it changed just now too.
    renderBindings();
    renderKnobHint();

    // The chain bits are by position, so what they mean just changed
    renderAttention();

    // Front anchor, then the chain in order, then the back anchor
    const order = [];
    if (PEDAL_EFFECTS.length)
        order.push(PEDAL_EFFECTS[0].id);
    routeIds.forEach(id => order.push(id));
    if (PEDAL_EFFECTS.length > 1)
        order.push(PEDAL_EFFECTS[PEDAL_EFFECTS.length - 1].id);

    const placed = new Set(order);

    order.forEach(id => {
        const card = effectCards.get(id);
        if (!card)
            return;
        effectsContainer.appendChild(card);
        card.classList.remove('parked');

        //
        // Anything drawn from the card's own measurements was drawn
        // blind while it was parked, because a hidden element has no
        // size to measure.  This is not a corner case: the pedal's state
        // dump arrives before the routing order that ends it, so every
        // card is parked for the whole of it.
        //
        const effect = PEDAL_EFFECTS[effectIdMap.get(id)];
        if (effect && effect.redrawCurve)
            effect.redrawCurve();

        // Open it on the way in.  An effect that has just been added is
        // one you are about to set up - but only on the way in, or
        // reordering the chain would keep reopening a card you closed.
        if (!wasRouted.has(id) && !isAnchorEffect(effectIdMap.get(id)))
            setCardCollapsed(card, false);
    });

    PEDAL_EFFECTS.forEach(effect => {
        const card = effectCards.get(effect.id);
        if (!card || placed.has(effect.id))
            return;
        effectsContainer.appendChild(card);
        card.classList.add('parked');
    });

    if (effectPool) {
        effectsContainer.appendChild(effectPool);
        renderPool();
    }
}

//
// The pool: everything routable that is not in the chain.
//
// A set, not a list.  The firmware resets an effect the moment it leaves
// the chain, so an unrouted effect has no values and no position - there
// is nothing here to order and nothing to edit. Full cards said otherwise
// on both counts, offering a drag that was discarded on release and
// sliders whose values were thrown away by the next routing change.
//
// Chips are rebuilt from scratch each time because they hold nothing: a
// name and an id, both of which came from the schema.
//
function renderPool() {
    if (!effectPool)
        return;

    effectPool.innerHTML = '';

    const full = currentRouting.length >= MAX_ROUTED;
    const chips = [];

    PEDAL_EFFECTS.forEach((effect, idx) => {
        if (isAnchorEffect(idx) || currentRouting.includes(effect.id))
            return;

        const chip = document.createElement('button');
        chip.className = 'effect-chip';
        chip.textContent = effect.name;
        chip.dataset.effectId = effect.id;
        chip.disabled = full;
        chip.addEventListener('click', () => routeEffect(effect.id));
        chips.push(chip);
    });

    // Nothing left out of the chain, so nothing to say
    effectPool.classList.toggle('hidden', chips.length === 0);
    if (!chips.length)
        return;

    const label = document.createElement('div');
    label.className = 'pool-label';
    label.textContent = full
        ? `Chain is full at ${MAX_ROUTED} — remove one to add another`
        : 'Not in the chain — tap to add';
    effectPool.appendChild(label);

    const grid = document.createElement('div');
    grid.className = 'pool-chips';
    chips.forEach(chip => grid.appendChild(chip));
    effectPool.appendChild(grid);
}


// Drag styling, kept next to the code that relies on it
const style = document.createElement('style');
style.textContent = `
    .effect-card.dragging {
        position: fixed;
        z-index: 1000;
        margin: 0;
        pointer-events: none;
        cursor: grabbing;
        box-shadow: 0 12px 32px rgba(0, 0, 0, 0.45);
    }
    .drag-placeholder {
        box-sizing: border-box;
        border: 2px dashed var(--primary);
        border-radius: 12px;
        opacity: 0.4;
    }
`;
document.head.appendChild(style);

function renderUI() {
    effectsContainer.innerHTML = '';
    effectCards.clear();

    effectPool = document.createElement('div');
    effectPool.className = 'effect-pool';
    effectPool.id = 'effect-pool';

    PEDAL_EFFECTS.forEach((effect, idx) => {
        const card = document.createElement('section');
        card.className = 'glass-panel effect-card';
        card.id = `effect-${idx}`;

        // Header
        const header = document.createElement('div');
        header.className = 'effect-header';

        card.dataset.effectId = effect.id;

        const title = document.createElement('div');
        title.className = 'effect-title';
        title.style.display = 'flex';
        title.style.alignItems = 'center';

        // The two anchors are not in the chain, so there is nothing to
        // reorder them relative to and no handle on them
        if (!isAnchorEffect(idx)) {
            title.innerHTML = `<span class="drag-handle">≡</span>
                               <span class="collapse-chevron" style="cursor: pointer; margin-right: 8px; font-size: 0.8em; transition: transform 0.2s;">▼</span>
                               <span>${effect.name}</span>`;

            // A drag starts on the header and nowhere else, so there is
            // never any question of whether you meant the card or the
            // slider you happen to be over - and the header is big
            // enough to hit with a thumb, which the handle was not.
            // The controls that sit in the header keep their own
            // presses; cardDragStart() steps out of the way for them.
            header.classList.add('draggable');
            header.addEventListener('pointerdown',
                                    (e) => cardDragStart(card, header, e));
            // The other half of the header's job, and the two never
            // both engage: one is the handle and a mouse, the other is
            // a finger anywhere else
            header.addEventListener('pointerdown',
                                    (e) => cardSwipeStart(card, e));
        } else {
            title.innerHTML = `<span class="collapse-chevron" style="cursor: pointer; margin-right: 8px; font-size: 0.8em; transition: transform 0.2s;">▼</span>
                               <span>${effect.name}</span>`;
        }

        const enableGroup = document.createElement('div');
        enableGroup.className = 'control-group enable-group';
        enableGroup.innerHTML = `
            <button class="action-btn effect-reset-btn" title="Reset to Defaults">↺</button>
        `;

        //
        // Out of the chain, and back to being a chip.  The counterpart of
        // tapping the chip, and the reason neither direction needs a drag
        // any more.
        //
        // cardDragStart() steps aside for anything with .action-btn on
        // it, so this does not have to fight the header's drag.
        //
        if (!isAnchorEffect(idx)) {
            const unrouteBtn = document.createElement('button');
            unrouteBtn.className = 'action-btn effect-unroute-btn';
            unrouteBtn.title = 'Remove from the chain';
            unrouteBtn.textContent = '⏏';
            unrouteBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                unrouteEffect(effect.id);
            });
            enableGroup.appendChild(unrouteBtn);
        }

        header.appendChild(title);
        header.appendChild(enableGroup);
        card.appendChild(header);

        // Collapse toggle
        const chevron = title.querySelector('.collapse-chevron');
        if (chevron) {
            chevron.addEventListener('click', (e) => {
                e.stopPropagation();
                const controls = card.querySelector('.effect-controls');
                setCardCollapsed(card, controls.style.display !== 'none');
            });
        }

        // The Reset button resets all pots. We should also reset the Mix pot!
        const resetBtn = enableGroup.querySelector('.effect-reset-btn');
        resetBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            effect.pots.forEach((potDef, pIdx) => {
                const initialVal = getInitialPotValue(potDef);
                const inputEl = ccToElementMap.get(`eff-${idx}-pot-${pIdx}`);
                if (inputEl) {
                    inputEl.value = initialVal;
                    inputEl.dispatchEvent(new Event(inputEl.tagName === 'SELECT' ? 'change' : 'input'));
                }
            });
            const mixEl = ccToElementMap.get(`eff-${idx}-mix`);
            if (mixEl) {
                const defaultMixVal = Math.round((effect.defMix !== undefined ? effect.defMix : 1.0) * 120);
                mixEl.value = defaultMixVal;
                mixEl.dispatchEvent(new Event('input'));
            }
        });

        // Controls
        const controls = document.createElement('div');

        let slidersContainer = null;
        let eqPotsInputs = [];
        let eqFooter = null;

        //
        // Some effects are better drawn than listed, because frequency
        // and gain per band is two-dimensional and a row of
        // one-dimensional controls cannot say it.
        //
        // Which ones is not this code's business any more.  The effect
        // header declares its band list and the schema carries it, so
        // the app asks what an effect is rather than which effect it is
        // - it used to check the display name, and renaming that effect
        // silently turned its curve into ten sliders.
        //
        // Bands take the pots in order, two each: frequency then gain.
        //
        const bands = (effect.graph || []).map((band, b) => ({
            type: band.type, q: band.q, qPot: band.qPot,
            freqPot: b * 2, gainPot: b * 2 + 1
        }));
        const isEq = bands.length > 0;

        if (isEq) {

            controls.className = 'effect-controls eq-container';

            const curveWrapper = document.createElement('div');
            curveWrapper.className = 'eq-curve-wrapper';
            curveWrapper.innerHTML = `
                <canvas id="eq-canvas-${idx}" width="1000" height="300" class="eq-canvas"></canvas>
            `;
            controls.appendChild(curveWrapper);

            slidersContainer = document.createElement('div');
            slidersContainer.className = 'eq-sliders';
            controls.appendChild(slidersContainer);

            //
            // The row under the curve.  The Mix control is the only pot
            // this effect shows - the other ten are the graph - so it
            // had the whole width to itself and did not need it.
            //
            eqFooter = document.createElement('div');
            eqFooter.className = 'eq-footer';
            controls.appendChild(eqFooter);

            effect.redrawCurve = () => {
                const canvas = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                //
                // The pots are collected as the card is built, so an
                // early redraw can arrive before they are all there.
                // Two per band is how many it takes to draw one.
                //
                if (!canvas || eqPotsInputs.length < bands.length * 2) return;
                const ctx = canvas.getContext('2d');
                const W = canvas.width;
                const H = canvas.height;
                ctx.clearRect(0, 0, W, H);

                //
                // How many canvas units there are to a screen pixel, for
                // the things that should be a fixed size to the eye.
                //
                // A parked card has no layout and so no width at all.
                // Falling back to 1 keeps this finite; applyRouting()
                // redraws when a card is unparked, which is what makes
                // the fallback temporary rather than wrong - a card is
                // parked for the whole of the pedal's state dump, so
                // this is the ordinary case at startup and not a corner.
                //
                const rect = canvas.getBoundingClientRect();
                const scaleX = rect.width ? W / rect.width : 1;
                const scaleY = rect.height ? H / rect.height : 1;

                // Math helper for biquad mag sq
                function fastsincos(f) {
                    const rad = f * 2 * Math.PI;
                    return { sin: Math.sin(rad), cos: Math.cos(rad) };
                }
                function pow2(x) { return Math.pow(2, x); }
                function biquad_mag_sq(c, w0, w2) {
                    const re_num = c.b0 + c.b1 * w0.cos + c.b2 * w2.cos;
                    const im_num = c.b1 * w0.sin + c.b2 * w2.sin;
                    const num = re_num * re_num + im_num * im_num;

                    const re_den = 1.0 + c.a1 * w0.cos + c.a2 * w2.cos;
                    const im_den = c.a1 * w0.sin + c.a2 * w2.sin;
                    const den = re_den * re_den + im_den * im_den;

                    if (den < 1e-12) return num * 1e12;
                    return num / den;
                }
                function biquad_loshelf(w0, Q, A) {
                    const alpha = w0.sin / (2 * Q);
                    const ap1 = A + 1, am1 = A - 1;
                    const sqAmin2 = 2 * Math.sqrt(A) * alpha;
                    const a0_inv = 1 / (ap1 + am1 * w0.cos + sqAmin2);
                    return {
                        b0: A * (ap1 - am1 * w0.cos + sqAmin2) * a0_inv,
                        b1: 2 * A * (am1 - ap1 * w0.cos) * a0_inv,
                        b2: A * (ap1 - am1 * w0.cos - sqAmin2) * a0_inv,
                        a1: -2 * (am1 + ap1 * w0.cos) * a0_inv,
                        a2: (ap1 + am1 * w0.cos - sqAmin2) * a0_inv
                    };
                }
                function biquad_peaking(w0, Q, A) {
                    const alpha = w0.sin / (2 * Q);
                    const a0_inv = 1 / (1 + alpha / A);
                    return {
                        b0: (1 + alpha * A) * a0_inv,
                        b1: (-2 * w0.cos) * a0_inv,
                        b2: (1 - alpha * A) * a0_inv,
                        a1: (-2 * w0.cos) * a0_inv,
                        a2: (1 - alpha / A) * a0_inv
                    };
                }
                function biquad_hishelf(w0, Q, A) {
                    const alpha = w0.sin / (2 * Q);
                    const ap1 = A + 1, am1 = A - 1;
                    const sqAmin2 = 2 * Math.sqrt(A) * alpha;
                    const a0_inv = 1 / (ap1 - am1 * w0.cos + sqAmin2);
                    return {
                        b0: A * (ap1 + am1 * w0.cos + sqAmin2) * a0_inv,
                        b1: -2 * A * (am1 + ap1 * w0.cos) * a0_inv,
                        b2: A * (ap1 + am1 * w0.cos - sqAmin2) * a0_inv,
                        a1: 2 * (am1 - ap1 * w0.cos) * a0_inv,
                        a2: (ap1 - am1 * w0.cos - sqAmin2) * a0_inv
                    };
                }

                const pots = eqPotsInputs.map(el => parseInt(el.value));
                const getFloat = (p_idx) => potToValue(effect.pots[p_idx], pots[p_idx]);
                //
                // A, not the gain: 10^(dB/40) is the square root of the
                // linear gain, which is what the cookbook formulas above
                // want and what they are handed here.
                //
                // The firmware's _biquad_peaking() takes the *gain* in
                // that argument and square-roots it itself, so it is
                // handed db_to_level(), which is 10^(dB/20).  Same name,
                // same shape, fourth argument in different units - the
                // two agree, and they agree by arriving from opposite
                // directions.  Making them "consistent" without checking
                // which one is which would double or halve every dB in
                // the picture.
                //
                function peq_pot_A(db) { return Math.pow(10, db / 40.0); }

                //
                // Q comes from the band, not from here.  It used to be
                // 1.0 for everything the app drew while the effect ran
                // whatever its own header said, so the curve was not the
                // filter for anything that disagreed.
                //
                const fs = 48000;
                const shape = { LOSHELF: biquad_loshelf, PEAKING: biquad_peaking,
                                HISHELF: biquad_hishelf };
                // A band's Q is a fixed number, or a pot it can be
                // dragged from while playing
                const bandQ = (band) =>
                      band.qPot === undefined ? band.q : getFloat(band.qPot);

                const coeff = bands.map((band) =>
                      shape[band.type](fastsincos(getFloat(band.freqPot) / fs),
                                       bandQ(band), peq_pot_A(getFloat(band.gainPot))));

                //
                // The phase, behind everything, if anybody wants it.
                //
                // biquad_mag_sq() builds the same two complex numbers
                // and throws their angles away, so this costs an atan2
                // apiece and nothing else.  The sign matters here where
                // it did not there: z is exp(-jw), so the imaginary
                // parts are negative, and a magnitude does not care.
                //
                // Wrapped to +-180 rather than unwrapped.  Five sections
                // can run through several turns and an unwrapped plot
                // would rescale itself as you drag, which is exactly the
                // sort of thing that pulls the eye away from the curve
                // that matters.  The cost is a jump at each wrap, so the
                // path is broken there instead of drawn through.
                //
                function biquad_phase(c, w0, w2) {
                    const num = Math.atan2(-(c.b1 * w0.sin + c.b2 * w2.sin),
                                           c.b0 + c.b1 * w0.cos + c.b2 * w2.cos);
                    const den = Math.atan2(-(c.a1 * w0.sin + c.a2 * w2.sin),
                                           1 + c.a1 * w0.cos + c.a2 * w2.cos);
                    return num - den;
                }

                if (eqShowPhase) {
                    const phaseToY = (deg) => H / 2 * (1 - deg / 180);
                    let last = null;

                    ctx.beginPath();
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = 'rgba(255, 255, 255, 0.16)';

                    for (let x = 0; x <= W; x += 2) {
                        let freq = xToFreq(x, W);
                        if (freq < 5) freq = 5;
                        if (freq > 24000) freq = 24000;

                        const w0 = fastsincos(freq / fs);
                        const w2 = fastsincos((2.0 * freq) / fs);

                        let rad = 0;
                        for (const c of coeff)
                            rad += biquad_phase(c, w0, w2);

                        const deg = ((rad * 180 / Math.PI + 180) % 360 + 360) % 360 - 180;
                        const y = phaseToY(deg);

                        // A wrap is a jump of half the plot or more, and
                        // is not a line the filter ever draws
                        if (last === null || Math.abs(deg - last) > 180)
                            ctx.moveTo(x, y);
                        else
                            ctx.lineTo(x, y);
                        last = deg;
                    }
                    ctx.stroke();
                }

                ctx.beginPath();
                ctx.lineWidth = 4;
                ctx.strokeStyle = '#4ecca3';

                for (let x = 0; x <= W; x += 2) {
                    let freq = xToFreq(x, W);
                    // clamp freq for math stability outside margins
                    if (freq < 5) freq = 5;
                    if (freq > 24000) freq = 24000;

                    const w0 = fastsincos(freq / fs);
                    const w2 = fastsincos((2.0 * freq) / fs);

                    let mag_sq = 1.0;
                    for (const c of coeff)
                        mag_sq *= biquad_mag_sq(c, w0, w2);

                    let mag = Math.sqrt(mag_sq);
                    if (mag < 0.0001) mag = 0.0001;
                    const db = 20.0 * Math.log10(mag);

                    // map dB to Y [0..H]
                    let y = dbToY(db, H);

                    if (x === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                }
                ctx.stroke();

                // Draw interactive nodes
                nodes = [];
                for (let i = 0; i < bands.length; i++) {
                    const freq = getFloat(bands[i].freqPot);
                    const db = getFloat(bands[i].gainPot);

                    const x = freqToX(freq, W);
                    const y = dbToY(db, H);

                    nodes.push({x, y});

                    //
                    // The node and its labels are drawn in screen px,
                    // not canvas units.
                    //
                    // The canvas is 1000 wide and 300 tall and is drawn
                    // at whatever width the card is, so the two axes are
                    // scaled by quite different amounts and a round arc
                    // is round at exactly one card width.  On a phone
                    // the dot came out about 2.5px across and 8px tall,
                    // its outline was three times thinner sideways than
                    // it was top to bottom, and the labels were squashed
                    // to a third of their width.
                    //
                    // Scaling by exactly what the css then divides by
                    // leaves the net transform an identity, so anything
                    // drawn in here lands on the glass the size it says.
                    // It has to wrap the strokes and the text too: a
                    // line width is in user units like everything else,
                    // which is the whole reason the outline was wrong.
                    //
                    // The curve itself is deliberately left alone.  It
                    // is a graph, and a graph is drawn in the units of
                    // the thing it is plotting.  These are controls
                    // sitting on top of it, and a control's size is a
                    // claim about where it can be grabbed - so it wants
                    // to agree with the hit test, which is also in
                    // screen px.  See EQ_GRAB_RADIUS.
                    //
                    const colour = (typeof activeNodeIdx !== 'undefined'
                                    && i === activeNodeIdx) ? '#ffffff' : '#4ecca3';

                    ctx.save();
                    ctx.translate(x, y);
                    ctx.scale(scaleX, scaleY);

                    //
                    // Which kind of band this is, said with a tick
                    // rather than a colour.
                    //
                    // A shelf runs flat away from its corner frequency
                    // and a peak does not, so a tick pointing the way
                    // the shelf acts - left for the low one, right for
                    // the high one - is a small picture of what the band
                    // does, and needs no legend to read.  Colour would
                    // have needed one, and the fill is already spoken
                    // for: it says which node you have hold of.
                    //
                    // Drawn before the dot so the dot covers its root.
                    // Which way it points comes from the band's own
                    // kind, so an effect with a different arrangement of
                    // sections gets the right ticks without being known
                    // about here.
                    //
                    const shelf = bands[i].type === 'LOSHELF' ? -1
                                : bands[i].type === 'HISHELF' ? 1 : 0;
                    if (shelf) {
                        ctx.beginPath();
                        ctx.moveTo(0, 0);
                        ctx.lineTo(shelf * EQ_SHELF_TICK, 0);
                        ctx.lineCap = 'round';

                        //
                        // Stroked twice, for the dark edge the dot gets
                        // and for the same reason.  A node is drawn in
                        // the curve's own colour, which is deliberate -
                        // the dot survives it by having an outline, and
                        // a bare tick did not: it lay along a flat
                        // stretch of curve and disappeared into it,
                        // which next to a shelf at 0dB is most of the
                        // time.
                        //
                        // Round caps so the far end is edged too, and
                        // the near end is under the dot either way.
                        //
                        ctx.lineWidth = EQ_SHELF_TICK_WIDTH + 2;
                        ctx.strokeStyle = '#1a1a2e';
                        ctx.stroke();

                        ctx.lineWidth = EQ_SHELF_TICK_WIDTH;
                        ctx.strokeStyle = colour;
                        ctx.stroke();
                    }

                    ctx.beginPath();
                    ctx.arc(0, 0, EQ_NODE_RADIUS, 0, 2 * Math.PI);
                    ctx.fillStyle = colour;
                    ctx.fill();
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = '#1a1a2e';
                    ctx.stroke();

                    // Draw labels
                    ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
                    ctx.font = '12px "Inter", sans-serif';
                    ctx.textAlign = 'center';
                    ctx.fillText(formatFreqShort(freq), 0, -24);
                    ctx.fillText(`${db > 0 ? '+' : ''}${db.toFixed(1)}dB`, 0, -10);

                    ctx.restore();
                }
            };



            // Interactive EQ logic
            //
            // Both in screen px.  The dot is what you see and the radius
            // is what you can hit, and a target wider than its dot is
            // the normal arrangement - nothing else on this canvas is
            // grabbable, so there is nothing for it to steal from, and
            // the nearest node wins where two overlap.
            //
            const EQ_NODE_RADIUS = 7;
            const EQ_GRAB_RADIUS = 30;
            const EQ_SHELF_TICK = 14;   // how far a shelf's tick reaches out
            const EQ_SHELF_TICK_WIDTH = 3;

            //
            // How much of the gain axis the graph shows, either side of
            // 0dB.  More than the pots can reach, which is +-20.
            //
            // They used to be the same number, so a band at full gain
            // put its node exactly on the canvas edge - the dot drawn
            // half outside it and the two labels, which sit above the
            // node, entirely outside it.  The reading disappeared at
            // precisely the setting you would want to read.
            //
            // Widening the axis rather than making the canvas taller.
            // Taller costs vertical space on the screen that a phone
            // does not have, and the picture does not want it: what is
            // gained is headroom, which is empty by definition. This
            // way +-20dB is the middle 71% of the height and the rest
            // is room for the labels to live in.  It also means the
            // summed curve, which five bands can push well past 20dB,
            // stays in the picture for longer.
            //
            const EQ_DB_HEADROOM = 8;
            const EQ_DB_SPAN = EQ_DB_HEADROOM +
                  Math.max(...bands.map((b) => Math.max(Math.abs(effect.pots[b.gainPot].min),
                                                        Math.abs(effect.pots[b.gainPot].max))));

            const dbToY = (db, H) => H / 2 - db * (H / (2 * EQ_DB_SPAN));
            const yToDb = (y, H) => (H / 2 - y) * (2 * EQ_DB_SPAN) / H;

            //
            // The frequency axis, taken from the pots rather than
            // written down a second time.
            //
            // The two have to span the same range or a band at the end
            // of its travel sits off the end of the picture, and they
            // were separately hardcoded at 20-20000 with nothing keeping
            // them that way.  The bands are the reason the graph exists,
            // so they are what it should measure.
            //
            const EQ_MARGIN = 40;
            const fMin = Math.min(...bands.map((b) => effect.pots[b.freqPot].min));
            const fMax = Math.max(...bands.map((b) => effect.pots[b.freqPot].max));
            const fSpan = Math.log2(fMax / fMin);

            const freqToX = (f, W) =>
                  EQ_MARGIN + Math.log2(f / fMin) / fSpan * (W - 2 * EQ_MARGIN);
            const xToFreq = (x, W) =>
                  fMin * Math.pow(2, (x - EQ_MARGIN) / (W - 2 * EQ_MARGIN) * fSpan);

            const EQ_PICK_SLOP = 4;     // px before a drag has a direction

            let isDragging = false;
            let dragPointerId = null;
            let dragStartX = 0, dragStartY = 0;
            let dragPile = null;
            let activeNodeIdx = -1;

            // The five band frequencies, as pot values - the app's live
            // copy of what the pedal has
            const freqPots = () =>
                  bands.map((b) => parseInt(eqPotsInputs[b.freqPot].value));
            let nodes = [];

            let lastEqUpdate = 0;
            // Helper to inverse map freq/db to MIDI val (0-120)
            function updateEqNode(nodeIdx, freq, db) {
                const fIdx = bands[nodeIdx].freqPot;
                const gIdx = bands[nodeIdx].gainPot;
                const fDef = effect.pots[fIdx];
                const gDef = effect.pots[gIdx];

                // Off the end of the graph is off the end of the pot,
                // which valueToPot() already answers by clamping
                const gVal = valueToPot(gDef, db);
                let fVal = valueToPot(fDef, freq);

                if (eqKeepOrder)
                    fVal = clampToNeighbours(freqPots(), nodeIdx, fVal);

                // Update inputs visually
                eqPotsInputs[fIdx].value = fVal;
                eqPotsInputs[gIdx].value = gVal;

                // Throttle SysEx to max 50Hz (20ms) to prevent USB floods, but always send
                const now = performance.now();
                if (now - lastEqUpdate > 20) {
                    sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, fIdx + 1, fVal]);
                    sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, gIdx + 1, gVal]);
                    lastEqUpdate = now;
                }

                const fDisplay = eqPotsInputs[fIdx].parentElement.querySelector('.pot-value');
                if (fDisplay) fDisplay.textContent = formatPotValue(fDef, fVal);

                const gDisplay = eqPotsInputs[gIdx].parentElement.querySelector('.pot-value');
                if (gDisplay) gDisplay.textContent = formatPotValue(gDef, gVal);

                effect.redrawCurve();
            }

            //
            // Where the pointer is, in canvas coordinates.
            //
            // Deliberately not clamped, and deliberately still answered
            // when the pointer is outside the canvas: a drag that has
            // left the graph is still a drag, and onMove() clamps what
            // it does with this rather than refusing to hear it.
            //
            const getPointerPos = (e) => {
                const canvas = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                const rect = canvas.getBoundingClientRect();
                const scaleX = canvas.width / rect.width;
                const scaleY = canvas.height / rect.height;
                return {
                    x: (e.clientX - rect.left) * scaleX,
                    y: (e.clientY - rect.top) * scaleY,
                    scaleX, scaleY
                };
            };

            const onDown = (e) => {
                if (!e.isPrimary)
                    return;
                e.preventDefault();
                const pos = getPointerPos(e);
                let minDist = Infinity;
                activeNodeIdx = -1;

                //
                // How close is close enough, measured on the screen
                // rather than on the canvas.
                //
                // The canvas is 1000 units wide and 300 tall, and it is
                // drawn at whatever width the card is with the height
                // fixed - so the two axes are scaled by quite different
                // amounts, and a radius in canvas units is an ellipse on
                // the glass. On a phone it came out about 12px wide and
                // 40px tall: tightest on the axis the bands are spread
                // along, on the device with the least precision, which
                // is exactly backwards.
                //
                // Dividing by the scale asks the question in the units
                // the finger is actually in, so the target is a circle
                // and it is the same size everywhere.
                //
                nodes.forEach((n, i) => {
                    const dx = (n.x - pos.x) / pos.scaleX;
                    const dy = (n.y - pos.y) / pos.scaleY;
                    const dist = Math.hypot(dx, dy);
                    if (dist < EQ_GRAB_RADIUS && dist < minDist) {
                        minDist = dist;
                        activeNodeIdx = i;
                    }
                });
                if (activeNodeIdx !== -1) {
                    isDragging = true;
                    dragPointerId = e.pointerId;
                    dragStartX = e.clientX;
                    dragStartY = e.clientY;

                    //
                    // If bands are piled on top of each other, which
                    // one you meant depends on which way you are about
                    // to go, and that is not known yet.  Remember the
                    // pile and settle it on the first real movement.
                    //
                    // The provisional pick stays highlighted meanwhile
                    // and it does not matter which of them it is: they
                    // are drawn in the same place, so the correction is
                    // invisible.
                    //
                    const pile = pileAt(freqPots(), activeNodeIdx);
                    dragPile = pile.length > 1 ? pile : null;
                    //
                    // The rest of the drag is heard on the window, not
                    // on the canvas.  A node has limits and the pointer
                    // does not, so the two part company at the edge of
                    // the graph all the time - and while they are apart,
                    // every move event would go to whatever the pointer
                    // is over instead of to us.
                    //
                    // That is what used to end the drag: moves stopped
                    // arriving, and 'mouseleave' called onUp on the way
                    // out for good measure. The node stopped where it
                    // was and you had to find it and grab it again,
                    // which with a finger means finding it underneath
                    // the finger.
                    //
                    window.addEventListener('pointermove', onMove);
                    window.addEventListener('pointerup', onUp);
                    window.addEventListener('pointercancel', onUp);
                    effect.redrawCurve();
                }
            };

            const onMove = (e) => {
                if (!isDragging || activeNodeIdx === -1) return;
                if (e.pointerId !== dragPointerId) return;
                e.preventDefault();

                //
                // Settle which of a pile was meant, once, on the first
                // movement worth reading - and change nothing at all
                // until then, so the answer cannot arrive after some
                // other band has already been edited.
                //
                // Going left takes the lowest-numbered and going right
                // the highest, which sounds like a convention and is
                // not: under clamping those are the only two that can
                // move at all.  The rest of the pile is blocked by the
                // one being picked.  So this is "take the one that is
                // free to go where you are going", and it peels a pile
                // apart one band at a time.
                //
                // A mostly-vertical first move is a gain edit, where
                // the order does not come into it, so the provisional
                // pick stands.
                //
                if (dragPile) {
                    const dx = e.clientX - dragStartX;
                    const dy = e.clientY - dragStartY;

                    if (Math.abs(dx) < EQ_PICK_SLOP && Math.abs(dy) < EQ_PICK_SLOP)
                        return;
                    if (Math.abs(dx) >= Math.abs(dy))
                        activeNodeIdx = dx < 0 ? dragPile[0]
                                               : dragPile[dragPile.length - 1];
                    dragPile = null;
                }

                const pos = getPointerPos(e);
                const canvas = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                const W = canvas.width;
                const H = canvas.height;

                const posX = Math.max(0, Math.min(W, pos.x));
                const posY = Math.max(0, Math.min(H, pos.y));

                const freq = xToFreq(posX, W);
                const db = yToDb(posY, H);

                updateEqNode(activeNodeIdx, freq, db);
            };

            const onUp = (e) => {
                if (e.pointerId !== dragPointerId)
                    return;

                window.removeEventListener('pointermove', onMove);
                window.removeEventListener('pointerup', onUp);
                window.removeEventListener('pointercancel', onUp);
                dragPointerId = null;
                dragPile = null;

                if (isDragging && activeNodeIdx !== -1) {
                    // Force final sysex flush on release
                    const fIdx = bands[activeNodeIdx].freqPot;
                    const gIdx = bands[activeNodeIdx].gainPot;
                    const fVal = parseInt(eqPotsInputs[fIdx].value);
                    const gVal = parseInt(eqPotsInputs[gIdx].value);
                    if (!isNaN(fVal)) sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, fIdx + 1, fVal]);
                    if (!isNaN(gVal)) sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, gIdx + 1, gVal]);
                }
                isDragging = false;
                if (activeNodeIdx !== -1) {
                    activeNodeIdx = -1;
                    effect.redrawCurve();
                }
            };

            // We must wait for the canvas to be added to DOM
            setTimeout(() => {
                const canvasEl = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                if (canvasEl) {
                    // Only the grab is on the canvas - see onDown() for
                    // where the rest of it went and why
                    canvasEl.addEventListener('pointerdown', onDown);
                }
            }, 0);

        } else {
            controls.className = 'effect-controls';
        }

        // Generate Mix slider - the anchors have no wet and no dry
        if (!isAnchorEffect(idx)) {
            const mixPotDef = { name: 'Mix', curve: 'LINEAR', min: 0, max: 100, unit: '%' };
            const mixDiv = document.createElement('div');
            mixDiv.className = 'pot-control mix-pot-control';

            const mixLabel = document.createElement('div');
            mixLabel.className = 'pot-label';
            mixLabel.textContent = mixPotDef.name;

            const defaultMixVal = Math.round((effect.defMix !== undefined ? effect.defMix : 1.0) * 120);

            const mixValDisplay = document.createElement('div');
            mixValDisplay.className = 'pot-value';
            mixValDisplay.textContent = formatPotValue(mixPotDef, defaultMixVal);

            const mixInput = document.createElement('input');
            mixInput.type = 'range';
            mixInput.min = 0;
            mixInput.max = 120;
            mixInput.value = defaultMixVal;
            mixInput.potDef = mixPotDef;

            enableWheelAdjust(mixInput, mixDiv);
            ccToElementMap.set(`eff-${idx}-mix`, mixInput);
            mixInput.addEventListener('input', (e) => {
                const midiVal = parseInt(e.target.value);
                mixValDisplay.textContent = formatPotValue(mixPotDef, midiVal);
                sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, 0, midiVal]);
            });

            mixDiv.appendChild(mixLabel);
            mixDiv.appendChild(mixValDisplay);
            mixDiv.appendChild(mixInput);

            openOnTap(mixDiv, mixInput, () =>
                setActivePot(`eff-${idx}-mix`, mixPotDef,
                             parseInt(mixInput.value), effect.name));

            // The EQ puts it in a row with its own switches
            (eqFooter || controls).appendChild(mixDiv);
        }

        effect.pots.forEach((pot, pIdx) => {
            const potIdKey = `eff-${idx}-pot-${pIdx}`;

            // Whether the graph draws a node for this one, which
            // decides where it goes rather than how it looks
            const banded = isEq && pIdx < bands.length * 2;

            const potDiv = document.createElement('div');
            potDiv.className = 'pot-control';

            const label = document.createElement('div');
            label.className = 'pot-label';
            label.textContent = pot.name;

            //
            // What the control is for, if the effect said.  On the whole
            // control rather than the label, because the slider is the
            // part you are already pointing at when you wonder.
            //
            // The pedal supplies this along with everything else it
            // reports about itself, so a firmware that gains a pot gains
            // its explanation too, without the app being redeployed.
            //
            if (pot.info) {
                potDiv.title = `${pot.name} — ${pot.info}`;
                potDiv.classList.add('has-info');
            }

            const initialVal = getInitialPotValue(pot);

            if (pot.curve === 'ENUM' && pot.enum) {
                const select = document.createElement('select');
                select.className = 'enum-select';
                pot.enum.forEach((optStr, idx) => {
                    const opt = document.createElement('option');
                    opt.value = idx;
                    opt.textContent = optStr;
                    select.appendChild(opt);
                });
                select.value = initialVal;

                ccToElementMap.set(potIdKey, select);
                select.addEventListener('change', (e) => {
                    const midiVal = parseInt(e.target.value);
                    sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, pIdx+1, midiVal]);
                });

                potDiv.appendChild(label);
                potDiv.appendChild(select);
            } else {
                const valDisplay = document.createElement('div');
                valDisplay.className = 'pot-value';
                valDisplay.textContent = formatPotValue(pot, initialVal);

                const input = document.createElement('input');
                input.type = 'range';
                input.min = 0;
                input.max = 120;
                input.value = initialVal;
                input.potDef = pot; // Attach pot definition for formatting

                // Every pot of a graphed effect feeds the curve, node
                // or no node - see bandQ() - so every one redraws it
                if (isEq) {
                    input.redrawCurve = effect.redrawCurve;
                    eqPotsInputs.push(input);
                }

                enableWheelAdjust(input, potDiv);
                ccToElementMap.set(potIdKey, input);
                input.addEventListener('input', (e) => {
                    const midiVal = parseInt(e.target.value);
                    valDisplay.textContent = formatPotValue(pot, midiVal);
                    sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, pIdx+1, midiVal]);
                    if (input.redrawCurve) input.redrawCurve();
                });

                potDiv.appendChild(label);
                potDiv.appendChild(valDisplay);
                potDiv.appendChild(input);

                //
                // Tapping the pot opens the big slider panel - except for
                // a mouse grab of the inline slider itself, which is
                // someone dragging it, and having the panel and its
                // backdrop appear on top mid-drag is no help to anybody.
                //
                // A tap with a finger opens it wherever it lands, the
                // inline slider included: that slider is about 100px wide
                // for 121 values, which is not something a thumb can
                // aim at, and the panel is what it has instead.
                //
                openOnTap(potDiv, input, () =>
                    setActivePot(potIdKey, pot, parseInt(input.value), effect.name));
            }

            if (!isEq) {
                controls.appendChild(potDiv);
            } else if (pIdx < bands.length * 2) {
                //
                // Part of the picture.  The slider still exists and is
                // still what carries the value - the graph moves it -
                // but it is hidden, because the node is the control.
                //
                slidersContainer.appendChild(potDiv);
            } else {
                //
                // A pot the graph has no node for, so it needs somewhere
                // real to live: the row under the curve, beside Mix.
                // Hiding it with the others would have made it a control
                // that exists and cannot be reached.
                //
                eqFooter.appendChild(potDiv);
            }
        });

        if (isEq) {
            //
            // Switches for how the graph behaves.  Not pots, and not
            // sent anywhere: the pedal has no opinion about any of this
            // and no way to store it. They sit here rather than in the
            // app's menu because they are about this one card, and
            // because a control belongs next to the thing it governs.
            //
            const options = document.createElement('div');
            options.className = 'eq-options';

            const orderLabel = document.createElement('label');
            orderLabel.className = 'eq-option';
            orderLabel.title = 'Bands stop at their neighbours instead of ' +
                               'passing them. Off, the shelves and peaks can ' +
                               'be in any order, which the pedal is happy with.';

            const orderBox = document.createElement('input');
            orderBox.type = 'checkbox';
            orderBox.checked = eqKeepOrder;
            orderBox.addEventListener('change', () => {
                eqKeepOrder = orderBox.checked;
                setUiPref('eq.keepOrder', eqKeepOrder);
            });

            orderLabel.appendChild(orderBox);
            orderLabel.appendChild(document.createTextNode('Keep bands in order'));
            options.appendChild(orderLabel);

            const phaseLabel = document.createElement('label');
            phaseLabel.className = 'eq-option';
            phaseLabel.title = 'Draw the phase response behind the curve, ' +
                               'wrapped to +-180 degrees across the same height.';

            const phaseBox = document.createElement('input');
            phaseBox.type = 'checkbox';
            phaseBox.checked = eqShowPhase;
            phaseBox.addEventListener('change', () => {
                eqShowPhase = phaseBox.checked;
                setUiPref('eq.showPhase', eqShowPhase);
                effect.redrawCurve();
            });

            phaseLabel.appendChild(phaseBox);
            phaseLabel.appendChild(document.createTextNode('Phase'));
            options.appendChild(phaseLabel);
            eqFooter.appendChild(options);

            setTimeout(() => effect.redrawCurve(), 0);
        }
        card.appendChild(controls);

        //
        // The meters live on the Signal Chain card because that is where
        // the question gets asked: Level wants to sit above the noise
        // floor, and until now the only way to find the floor was to move
        // Level until something happened.
        //
        if (effect.base === 'signal_chain') {
            const meters = document.createElement('div');
            meters.className = 'meters';
            meters.id = 'signal-chain-meters';
            meters.textContent = 'no readings yet';
            card.appendChild(meters);
        }

        effectCards.set(effect.id, card);
        effectsContainer.appendChild(card);
    });

    //
    // Nothing is in the chain until the pedal says so, and the reply that
    // says so is still in flight.  Draw that rather than every card at
    // once: an effect the pedal is not running has no business looking
    // like one that is, even for a moment.
    //
    applyRouting([]);
}

appTitleEl.addEventListener('click', () => {
    if (appTitleEl.textContent.includes('Tap to Connect') || appTitleEl.textContent.includes('Error')) {
        appTitleEl.textContent = "Connecting...";
        initMidi();
    }
});

// Event Listeners
    const inSelect = document.getElementById('midi-input-select');
    if (inSelect) {
        inSelect.addEventListener('change', (e) => {
            selectedInputId = e.target.value;
            updateMidiState();
        });
    }
    const outSelect = document.getElementById('midi-output-select');
    if (outSelect) {
        outSelect.addEventListener('change', (e) => {
            selectedOutputId = e.target.value;
            updateMidiState();
        });
    }

    globalEnableEl.addEventListener('change', (e) => {
        sendMidiCc(GLOBAL_ENABLE_CC, e.target.checked ? 127 : 0);
    });

    const tunerBtn = document.getElementById('tuner-btn');
    if (tunerBtn) {
        tunerBtn.addEventListener('click', () => {
            isTunerMode = !isTunerMode;
            sendMidiCc(GLOBAL_ENABLE_CC, isTunerMode ? 68 : 69);
            updateTunerModeUI();
        });
    }

    const closeTunerBtn = document.getElementById('close-tuner-btn');
    if (closeTunerBtn) {
        closeTunerBtn.addEventListener('click', () => {
            isTunerMode = false;
            sendMidiCc(GLOBAL_ENABLE_CC, 69);
            updateTunerModeUI();
        });
    }

    const tunerSynthToggle = document.getElementById('tuner-synth-toggle');
    if (tunerSynthToggle) {
        tunerSynthToggle.addEventListener('change', (e) => {
            playSynth = e.target.checked;
            if (!playSynth) stopAllNotes();
            if (playSynth) getAudioContext(); // Initialize context if needed
        });
    }

    function closeMenu() {
        const menu = document.getElementById('global-menu');
        const burger = document.getElementById('burger-btn');
        if (menu) menu.classList.add('hidden');
        if (burger) burger.setAttribute('aria-expanded', 'false');
    }

    function closeAllPanels() {
        if (document.getElementById('panel-backdrop')) document.getElementById('panel-backdrop').classList.add('hidden');
        if (document.getElementById('settings-panel')) document.getElementById('settings-panel').classList.add('hidden');
        if (document.getElementById('bindings-panel')) document.getElementById('bindings-panel').classList.add('hidden');
        if (document.getElementById('active-pot-panel')) document.getElementById('active-pot-panel').classList.add('hidden');
        closeMenu();
        activePotCc = null;
        activePotDef = null;
    }

    const backdrop = document.getElementById('panel-backdrop');
    if (backdrop) {
        backdrop.addEventListener('click', closeAllPanels);
    }

    // A button whose label depends on something that can change while
    // the message is up rebuilds it rather than restoring whatever it
    // happened to say a second and a half ago.
    function restoreButton(btn, originalText) {
        if (btn.relabel)
            btn.relabel();
        else
            btn.innerHTML = originalText;
        btn.classList.remove('success', 'error');
    }

    //
    // Every one of these is a menu item, so success means the menu has
    // done its job and should go away.  It used to stay open, which made
    // sense when a round of testing meant several of these in a row and
    // reaching for the burger between each was the annoying part.  There
    // is nothing like that left, so now it just sits there afterwards
    // looking like it is waiting for something.
    //
    // Closing on the same timer that restores the label rather than
    // immediately: the label *is* the confirmation, and 'Saved to 3' is
    // worth reading before it goes.  Failures keep the menu open for the
    // same reason - showButtonError() does not close it, because the
    // error is the thing you need to see.
    //
    // A second, and not the second and a half it was.  Long enough to
    // read four words, and short enough that it never reads as the menu
    // having decided to stay open - which is what it looked like, having
    // spent so long doing exactly that.
    //
    const BUTTON_FEEDBACK_MS = 1000;

    function showButtonSuccess(btn, successText) {
        const originalText = btn.innerHTML;
        btn.innerHTML = `✓ ${successText}`;
        btn.classList.add('success');
        setTimeout(() => {
            restoreButton(btn, originalText);
            closeMenu();
        }, BUTTON_FEEDBACK_MS);
    }

    function showButtonError(btn, errorText) {
        const originalText = btn.innerHTML;
        btn.innerHTML = `⚠️ ${errorText}`;
        btn.classList.add('error');
        setTimeout(() => restoreButton(btn, originalText), BUTTON_FEEDBACK_MS);
    }

    // The burger menu
    const burgerBtn = document.getElementById('burger-btn');
    const globalMenu = document.getElementById('global-menu');

    if (burgerBtn && globalMenu) {
        burgerBtn.addEventListener('click', (e) => {
            // Or the document listener below sees this same click and
            // closes what we just opened.
            e.stopPropagation();
            const wasOpen = !globalMenu.classList.contains('hidden');
            closeAllPanels();
            if (!wasOpen) {
                globalMenu.classList.remove('hidden');
                burgerBtn.setAttribute('aria-expanded', 'true');
            }
        });

        // A menu goes away when you press somewhere else, which the
        // backdrop used to do for us and a dropdown has no backdrop for.
        document.addEventListener('click', (e) => {
            if (!globalMenu.classList.contains('hidden') &&
                !globalMenu.contains(e.target))
                closeMenu();
        });
    }

    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape')
            closeAllPanels();
    });

    // Settings, which is the one menu item that opens something rather
    // than doing something.
    const openSettingsBtn = document.getElementById('open-settings-btn');
    const settingsPanel = document.getElementById('settings-panel');
    const closeSettingsBtn = document.getElementById('close-settings');

    if (openSettingsBtn && settingsPanel) {
        openSettingsBtn.addEventListener('click', () => {
            closeAllPanels();
            settingsPanel.classList.remove('hidden');
            if (backdrop) backdrop.classList.remove('hidden');
        });
    }

    if (closeSettingsBtn) {
        closeSettingsBtn.addEventListener('click', closeAllPanels);
    }

    // Active Pot initialization
    const openBindingsBtn = document.getElementById('open-bindings-btn');
    if (openBindingsBtn) {
        openBindingsBtn.addEventListener('click', () => {
            closeAllPanels();
            renderBindings();
            document.getElementById('bindings-panel').classList.remove('hidden');
            if (backdrop) backdrop.classList.remove('hidden');
        });
    }

    const closeBindingsBtn = document.getElementById('close-bindings');
    if (closeBindingsBtn)
        closeBindingsBtn.addEventListener('click', closeAllPanels);

    //
    // Point the knob at whatever pot the panel is showing.  The hint
    // beside it says what is being taken away, because with one knob
    // every assignment takes it off something else.
    //
    const assignKnobBtn = document.getElementById('assign-knob-btn');
    if (assignKnobBtn) {
        assignKnobBtn.addEventListener('click', () => {
            const t = activePotTarget();
            if (!t)
                return;
            //
            // Move the knob's first rule rather than adding another,
            // since "assign" means point it here and a second rule
            // would mean drive both.
            //
            const i = pedalRules.findIndex(r => r.control === 0 &&
                                                r.action === ACT.POT);
            const r = { control: 0, action: ACT.POT,
                        effect: t.effId, pot: t.pot, val: [0, 0] };
            if (i < 0)
                sendRules(pedalRules.concat([r]));
            else
                putRule(i, r);
        });
    }

    const closeActivePotBtn = document.getElementById('close-active-pot');
    if (closeActivePotBtn) {
        closeActivePotBtn.addEventListener('click', () => {
            closeAllPanels();
        });
    }

    const activePotSlider = document.getElementById('active-pot-slider');
    if (activePotSlider) {
        // the whole panel: its name and readout are part of the pot too
        enableWheelAdjust(activePotSlider,
                          document.getElementById('active-pot-panel') || activePotSlider);
        activePotSlider.addEventListener('input', (e) => {
            if (activePotCc === null || !activePotDef) return;

            const val = parseInt(e.target.value);
            const valDisplay = document.getElementById('active-pot-value');

            if (valDisplay)
                valDisplay.textContent = formatPotValue(activePotDef, val);

            // Update original element
            const origInput = ccToElementMap.get(activePotCc);
            if (origInput) {
                origInput.value = val;
                const valDisplay = origInput.parentElement.querySelector('.pot-value');
                if (valDisplay) valDisplay.textContent = formatPotValue(activePotDef, val);
                if (origInput.redrawCurve) origInput.redrawCurve();
            }

            // Parse activePotCc to get effectId and potIdx
            // activePotCc is like "eff-2-pot-0" or "eff-2-mix"
            const parts = activePotCc.split('-');
            if (parts.length >= 4 && parts[2] === 'pot') {
                const idx = parseInt(parts[1]);
                const pIdx = parseInt(parts[3]);
                const effId = PEDAL_EFFECTS[idx].id;
                sendSysex([SYSEX_CMD.PARAM_UPDATE, effId, pIdx + 1, val]);
            } else if (parts.length === 3 && parts[2] === 'mix') {
                const idx = parseInt(parts[1]);
                const effId = PEDAL_EFFECTS[idx].id;
                sendSysex([SYSEX_CMD.PARAM_UPDATE, effId, 0, val]);
            }
        });
    }

    function setActivePot(cc, potDef, currentVal, effectName) {
        const panel = document.getElementById('active-pot-panel');
        const name = document.getElementById('active-pot-title');
        const valDisplay = document.getElementById('active-pot-value');

        // No panel, nothing to make active - and in particular don't set
        // activePotCc, which everything else takes as "the panel is up".
        if (!panel)
            return;

        closeAllPanels();
        activePotCc = cc;
        activePotDef = potDef;

        if (name) name.textContent = `${effectName} - ${potDef.name}`;
        if (valDisplay) valDisplay.textContent = formatPotValue(potDef, currentVal);
        if (activePotSlider) activePotSlider.value = currentVal;

        renderKnobHint();

        panel.classList.remove('hidden');
        if (backdrop) backdrop.classList.remove('hidden');
    }

    const globalUnrouteBtn = document.getElementById('global-unroute-btn');
    if (globalUnrouteBtn) {
        globalUnrouteBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalUnrouteBtn, 'Not Connected');
                return;
            }

            // A routing order with nothing in it unroutes everything. The
            // two anchors are never part of the routing order to begin
            // with - one always runs first, the other is not an effect.
            setRouting([]);
            showButtonSuccess(globalUnrouteBtn, 'All Unrouted');
        });
    }

    // The options are filled in from the identity reply, since only the
    // pedal knows how many scenes it has - see populateScenePicker().
    const sceneSelect = document.getElementById('global-scene-select');

    //
    // The picker is in the header and the two things that act on it are
    // in the menu, so the menu says which scene it means. Otherwise you
    // are trusting your memory of a control that isn't on screen while
    // you press the one that overwrites it.
    //
    const loadSceneBtn = document.getElementById('global-load-scene-btn');
    if (loadSceneBtn) {
        loadSceneBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(loadSceneBtn, 'Not Connected');
                return;
            }
            const sceneId = parseInt(sceneSelect.value);
            sendMidiPc(sceneId);
            setTimeout(() => {
                sendSysex([SYSEX_CMD.REQ_STATE]);
                sendSysex([SYSEX_CMD.DIAGNOSTIC]); // Check status after load
            }, 100);
            showButtonSuccess(loadSceneBtn, `Loaded ${sceneId}`);
        });
    }

    const saveSceneBtn = document.getElementById('global-save-scene-btn');
    if (saveSceneBtn) {
        saveSceneBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(saveSceneBtn, 'Not Connected');
                return;
            }
            const sceneId = parseInt(sceneSelect.value);
            sendSysex([SYSEX_CMD.SAVE_SCENE, sceneId]);
            sendSysex([SYSEX_CMD.DIAGNOSTIC]); // Check status after save
            showButtonSuccess(saveSceneBtn, `Saved to ${sceneId}`);
        });
    }

    if (sceneSelect) {
        sceneSelect.addEventListener('change', updateSceneLabels);
        if (loadSceneBtn) loadSceneBtn.relabel = updateSceneLabels;
        if (saveSceneBtn) saveSceneBtn.relabel = updateSceneLabels;
        updateSceneLabels();
    }

    const globalProgramBtn = document.getElementById('global-program-btn');
    if (globalProgramBtn) {
        globalProgramBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalProgramBtn, 'Not Connected');
                return;
            }
            // The confirmation is there because rebooting into the
            // bootloader in the middle of playing would be a nasty
            // surprise.  Developing against the thing, you do it over
            // and over on purpose, and the prompt is pure friction.
            if (!IS_LOCAL_DEV && !confirm("Reboot pedal into programming mode?"))
                return;
            sendMidiCc(GLOBAL_ENABLE_CC, 126);
            closeAllPanels();
        });
    }


// Boot
renderUI();
initMidi();
