const SYSEX_CMD = {
    REQ_SCHEMA: 0x01,
    RES_SCHEMA: 0x02,
    PARAM_UPDATE: 0x03,
    SAVE_SCENE: 0x04,
    REQ_STATE: 0x05,
    ROUTING_ORDER: 0x08,
    DIAGNOSTIC: 0x09
};

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
let activeTransmitChannel = 0xB0;

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
        if (!navigator.requestMIDIAccess) {
            appTitleEl.textContent = "Browser Not Supported";
            console.error("Web MIDI API is not supported in this browser.");
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

function populateMidiSelects() {
    const inSelect = document.getElementById('midi-input-select');
    const outSelect = document.getElementById('midi-output-select');
    if (!inSelect || !outSelect) return;

    inSelect.innerHTML = '<option value="">-- Auto-detect Linus Pedal --</option>';
    outSelect.innerHTML = '<option value="">-- Auto-detect Linus Pedal --</option>';

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

    console.log("[WebMIDI] updating MIDI state. Available inputs:");
    for (let input of midiAccess.inputs.values()) {
        console.log("  Input:", input.name, input.id);
    }
    console.log("[WebMIDI] Available outputs:");
    for (let output of midiAccess.outputs.values()) {
        console.log("  Output:", output.name, output.id);
    }

    if (selectedInputId && midiAccess.inputs.has(selectedInputId)) {
        foundInput = midiAccess.inputs.get(selectedInputId);
    } else {
        for (let input of midiAccess.inputs.values()) {
            if (!foundInput) foundInput = input;
            if (input.name.includes('Linus Pedal')) {
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
            if (output.name.includes('Linus Pedal')) {
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

        // Request initial state dump
        sendSysex([SYSEX_CMD.REQ_SCHEMA]);
        sendSysex([SYSEX_CMD.DIAGNOSTIC]); // Request diagnostic status
    } else {
        midiInput = null;
        midiOutput = null;
        appTitleEl.className = "title-disconnected";
        appTitleEl.textContent = "RP2350 Pedal";
    }
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
    if (data[0] !== SYSEX_CMD.DIAGNOSTIC && data[0] !== SYSEX_CMD.REQ_SCHEMA && data[0] !== SYSEX_CMD.REQ_STATE) {
        scheduleDiagnostic();
    }
}

let PEDAL_EFFECTS = [];
let effectIdMap = new Map();

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
                PEDAL_EFFECTS = JSON.parse(jsonStr);
                effectIdMap.clear();
                PEDAL_EFFECTS.forEach((e, idx) => effectIdMap.set(e.id, idx));
                renderUI();
                // Request State and Status
                sendSysex([SYSEX_CMD.REQ_STATE]);
                sendSysex([SYSEX_CMD.DIAGNOSTIC]);
            } catch (e) {
                console.error("Failed to parse schema", e);
            }
            break;
        }

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
            // Set Parameter
            if (data.length < 6) break;
            const effId = data[3];
            const potIdx = data[4];
            const val = data[5];

            const idx = effectIdMap.get(effId);
            if (idx !== undefined) {
                const idKey = potIdx === 0 ? `eff-${idx}-mix` : `eff-${idx}-pot-${potIdx-1}`;
                const el = ccToElementMap.get(idKey);
                if (el) {
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
            }
            break;
        }

        case SYSEX_CMD.ROUTING_ORDER: {
            // Routing order
            const routeIds = [];
            for (let i = 3; i < data.length - 1; i++) {
                routeIds.push(data[i]);
            }
            reorderEffectsInDOM(routeIds);

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
            if (val === 68) {
                isTunerMode = true;
                updateTunerModeUI();
            } else if (val === 69) {
                isTunerMode = false;
                updateTunerModeUI();
            } else if (val === 64 || val === 65 || val === 66 || val === 67 || val === 126) {
                if (val === 67) {
                    isGlobalEnabled = false;
                    globalEnableEl.checked = false;
                }
            } else {
                isGlobalEnabled = (val > 0);
                globalEnableEl.checked = isGlobalEnabled;
            }
        } else {
            if (cc === 107) { // MIDI Ch
                activeTransmitChannel = (val === 0) ? 0xB0 : (0xB0 | ((val - 1) & 0x0F));
            }
            // It's a pot or an effect enable
            const el = ccToElementMap.get(cc);
            if (el) {
                if (el.type === 'checkbox') {
                    el.checked = (val > 0);
                } else if (el.tagName === 'SELECT') {
                    el.value = val;
                } else if (el.type === 'range') {
                    el.value = val;
                    // Update display value if any
                    const valDisplay = el.parentElement.querySelector('.pot-value');
                    if (valDisplay && el.potDef) {
                        valDisplay.textContent = formatPotValue(el.potDef, val);
                    }
                    // If it has a redraw callback (like EQ), trigger it
                    if (el.redrawCurve) {
                        el.redrawCurve();
                    }

                    // Update active pot panel if it is the currently active one
                    if (cc === activePotCc && activePotDef) {
                        const activeSlider = document.getElementById('active-pot-slider');
                        if (activeSlider) activeSlider.value = val;
                        const activeValue = document.getElementById('active-pot-value');
                        if (activeValue) activeValue.textContent = formatPotValue(activePotDef, val);
                    }
                }
            }
        }
    } else if ((status & 0xF0) === 0xC0) {
        // Program Change (Active Effect)
        // Scroll to the active effect!
        const effectIdx = data1;
        if (effectIdx >= 0 && effectIdx < PEDAL_EFFECTS.length) {
            const effectCard = document.getElementById(`effect-${effectIdx}`);
            if (effectCard) {
                effectCard.scrollIntoView({ behavior: 'smooth', block: 'center' });
            }
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

    if (cc === 107) {
        activeTransmitChannel = (val === 0) ? 0xB0 : (0xB0 | ((val - 1) & 0x0F));
    }
    scheduleDiagnostic();
}

function formatPotValue(pot, val) {
    const p = val / 120.0;
    let y = val;

    if (pot.curve === 'RAW') {
        y = val;
    } else if (pot.curve === 'LINEAR') {
        y = pot.min + p * (pot.max - pot.min);
    } else if (pot.curve === 'FREQUENCY') {
        y = pot.min + (p * p * p) * (pot.max - pot.min);
    } else if (pot.curve === 'SQUARED') {
        y = pot.min + (p * p) * (pot.max - pot.min);
    } else if (pot.curve === 'EXPONENTIAL') {
        y = pot.min * Math.pow(pot.max / pot.min, p);
    }

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


function sendUpdatedRouting() {
    const cards = Array.from(effectsContainer.children);
    const routeIds = [];
    let isUnrouted = false;

    cards.forEach(card => {
        if (card.classList.contains('unrouted-divider')) {
            isUnrouted = true;
            return;
        }
        if (isUnrouted) return;

        const id = parseInt(card.dataset.effectId);
        const eff = PEDAL_EFFECTS.find(e => e.id === id);
        if (eff && eff.name !== 'Settings' && eff.name !== 'Noise Gate') {
            routeIds.push(id);
        }
    });

    const data = [SYSEX_CMD.ROUTING_ORDER, ...routeIds.slice(0, 14)];
    sendSysex(data);
    reorderEffectsInDOM(routeIds);
}


function getInitialPotValue(pot) {
    if (pot.default === undefined) return 60;
    const y = pot.default;

    if (pot.curve === 'RAW' || pot.curve === 'ENUM') return Math.round(y);

    let p = 0;
    const a = pot.min || 0;
    const b = pot.max || 1;

    if (pot.curve === 'LINEAR') {
        p = (b !== a) ? (y - a) / (b - a) : 0;
    } else if (pot.curve === 'FREQUENCY') {
        p = (b !== a) ? Math.pow((y - a) / (b - a), 1/3.0) : 0;
    } else if (pot.curve === 'SQUARED') {
        p = (b !== a) ? Math.pow((y - a) / (b - a), 0.5) : 0;
    } else if (pot.curve === 'EXPONENTIAL') {
        p = (b !== a && a !== 0 && y !== 0) ? Math.log2(y / a) / Math.log2(b / a) : 0;
    }

    let val = Math.round(p * 120);
    return Math.max(0, Math.min(120, val));
}


function reorderEffectsInDOM(routeIds) {
    const activeRouteIds = new Set(routeIds);
    const cards = Array.from(effectsContainer.children);
    cards.sort((a, b) => {
        const idA = a.classList.contains('unrouted-divider') ? 'div' : parseInt(a.dataset.effectId);
        const idB = b.classList.contains('unrouted-divider') ? 'div' : parseInt(b.dataset.effectId);

        let idxA, idxB;
        if (idA === 'div') {
            idxA = 998;
        } else {
            const effA = PEDAL_EFFECTS.find(e => e.id === idA);
            if (effA && effA.name === "Noise Gate") idxA = -2;
            else if (effA && effA.name === "Settings") idxA = 997;
            else idxA = routeIds.indexOf(idA) === -1 ? 999 : routeIds.indexOf(idA);
        }

        if (idB === 'div') {
            idxB = 998;
        } else {
            const effB = PEDAL_EFFECTS.find(e => e.id === idB);
            if (effB && effB.name === "Noise Gate") idxB = -2;
            else if (effB && effB.name === "Settings") idxB = 997;
            else idxB = routeIds.indexOf(idB) === -1 ? 999 : routeIds.indexOf(idB);
        }

        if (idxA !== idxB) return idxA - idxB;
        if (idA === 'div' || idB === 'div') return 0;
        return idA - idB;
    });

    cards.forEach(card => {
        effectsContainer.appendChild(card);
        if (card.classList.contains('unrouted-divider')) return;

        const id = parseInt(card.dataset.effectId);
        const isRouted = activeRouteIds.has(id);
        const eff = PEDAL_EFFECTS.find(e => e.id === id);
        const isAlwaysRouted = eff && (eff.name === "Noise Gate" || eff.name === "Settings");

        if (isRouted || isAlwaysRouted) {
            card.classList.remove('unrouted');
            // Auto expand when routed
            const controls = card.querySelector('.effect-controls');
            const chevron = card.querySelector('.collapse-chevron');
            if (controls) controls.style.display = '';
            if (chevron) chevron.style.transform = 'rotate(0deg)';
        } else {
            card.classList.add('unrouted');
            // Auto collapse unrouted effects
            const controls = card.querySelector('.effect-controls');
            const chevron = card.querySelector('.collapse-chevron');
            if (controls) controls.style.display = 'none';
            if (chevron) chevron.style.transform = 'rotate(-90deg)';
        }
    });
}


// Add some styles dynamically for drag and drop
const style = document.createElement('style');
style.textContent = `
    .effect-card.dragging {
        opacity: 0.5;
        border: 2px dashed var(--primary);
    }
`;
document.head.appendChild(style);

function renderUI() {
    effectsContainer.innerHTML = '';

    // Add unrouted divider
    const divider = document.createElement('div');
    divider.className = 'unrouted-divider';
    divider.textContent = '--- Unrouted Effects ---';

    divider.addEventListener('dragover', (e) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = 'move';
        const draggingCard = document.querySelector('.dragging');
        if (!draggingCard) return;
        divider.parentNode.insertBefore(draggingCard, divider.nextSibling);
    });

    divider.addEventListener('drop', (e) => {
        e.preventDefault();
        sendUpdatedRouting();
    });

    effectsContainer.appendChild(divider);

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

        // Settings effect cannot be reordered or collapsed (maybe collapsed is fine, but no drag)
        if (effect.name !== "Settings" && effect.name !== "Noise Gate") {
            title.innerHTML = `<span class="drag-handle" style="cursor: grab; margin-right: 12px; font-size: 1.4em; opacity: 0.7;">≡</span>
                               <span class="collapse-chevron" style="cursor: pointer; margin-right: 8px; font-size: 0.8em; transition: transform 0.2s;">▼</span>
                               <span>${effect.name}</span>`;

            // Enable dragging only when hovering the drag handle
            const dragHandle = title.querySelector('.drag-handle');
            if (dragHandle) {
                dragHandle.addEventListener('mouseenter', () => card.draggable = true);
                dragHandle.addEventListener('mouseleave', () => card.draggable = false);
                dragHandle.addEventListener('touchstart', () => card.draggable = true, {passive: true});
                dragHandle.addEventListener('touchend', () => card.draggable = false);
            }
        } else {
            title.innerHTML = `<span class="collapse-chevron" style="cursor: pointer; margin-right: 8px; font-size: 0.8em; transition: transform 0.2s;">▼</span>
                               <span>${effect.name}</span>`;
        }

        const enableGroup = document.createElement('div');
        enableGroup.className = 'control-group enable-group';
        enableGroup.innerHTML = `
            <button class="action-btn effect-reset-btn" title="Reset to Defaults">↺</button>
        `;

        header.appendChild(title);
        header.appendChild(enableGroup);
        card.appendChild(header);

        // Collapse toggle
        const chevron = title.querySelector('.collapse-chevron');
        if (chevron) {
            chevron.addEventListener('click', (e) => {
                e.stopPropagation();
                const controls = card.querySelector('.effect-controls');
                if (controls.style.display === 'none') {
                    controls.style.display = 'flex';
                    chevron.style.transform = 'rotate(0deg)';
                } else {
                    controls.style.display = 'none';
                    chevron.style.transform = 'rotate(-90deg)';
                }
            });
        }

        // Drag and drop logic
        card.addEventListener('dragstart', (e) => {
            e.dataTransfer.setData('text/plain', effect.id);
            e.dataTransfer.effectAllowed = 'move';
            card.classList.add('dragging');
        });

        card.addEventListener('dragend', (e) => {
            card.classList.remove('dragging');
        });

        card.addEventListener('dragover', (e) => {
            e.preventDefault();
            e.dataTransfer.dropEffect = 'move';
            const draggingCard = document.querySelector('.dragging');
            if (!draggingCard || draggingCard === card) return;

            if (effect.name === "Noise Gate") {
                card.parentNode.insertBefore(draggingCard, card.nextSibling);
                return;
            }
            if (effect.name === "Settings") {
                card.parentNode.insertBefore(draggingCard, card);
                return;
            }

            // Determine whether to insert before or after
            const rect = card.getBoundingClientRect();
            const midpoint = rect.top + rect.height / 2;
            if (e.clientY < midpoint) {
                card.parentNode.insertBefore(draggingCard, card);
            } else {
                card.parentNode.insertBefore(draggingCard, card.nextSibling);
            }
        });

        card.addEventListener('drop', (e) => {
            e.preventDefault();
            // Send new routing array via SysEx
            sendUpdatedRouting();
        });

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

        if (effect.name === 'Parametric EQ') {

            controls.className = 'effect-controls eq-container';

            const curveWrapper = document.createElement('div');
            curveWrapper.className = 'eq-curve-wrapper';
            curveWrapper.innerHTML = `
                <canvas id="eq-canvas-${idx}" width="1000" height="300" class="eq-canvas"></canvas>
            `;
            controls.appendChild(curveWrapper);

            slidersContainer = document.createElement('div');
            slidersContainer.className = 'eq-sliders eq-sliders-hidden';
            controls.appendChild(slidersContainer);

            effect.redrawCurve = () => {
                const canvas = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                if (!canvas || eqPotsInputs.length < 10) return;
                const ctx = canvas.getContext('2d');
                const W = canvas.width;
                const H = canvas.height;
                ctx.clearRect(0, 0, W, H);

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
                // getFloat maps 0-120 to actual value
                function getFloat(p_idx) {
                    const val = pots[p_idx];
                    const potDef = effect.pots[p_idx];
                    const p = val / 120.0;
                    if (potDef.curve === 'LINEAR') return potDef.min + p * (potDef.max - potDef.min);
                    if (potDef.curve === 'FREQUENCY') return potDef.min + Math.pow(p, 3) * (potDef.max - potDef.min);
                    return val;
                }
                function peq_pot_A(db) { return Math.pow(10, db / 40.0); }

                const fs = 48000;
                const Q = 1.0;
                const c0 = biquad_loshelf(fastsincos(getFloat(0)/fs), Q, peq_pot_A(getFloat(1)));
                const c1 = biquad_peaking(fastsincos(getFloat(2)/fs), Q, peq_pot_A(getFloat(3)));
                const c2 = biquad_peaking(fastsincos(getFloat(4)/fs), Q, peq_pot_A(getFloat(5)));
                const c3 = biquad_peaking(fastsincos(getFloat(6)/fs), Q, peq_pot_A(getFloat(7)));
                const c4 = biquad_hishelf(fastsincos(getFloat(8)/fs), Q, peq_pot_A(getFloat(9)));

                ctx.beginPath();
                ctx.lineWidth = 4;
                ctx.strokeStyle = '#4ecca3';

                const margin = 40;
                const graphWidth = W - 2 * margin;
                const maxFx = 13.0 * Math.log2(20000.0 / 20.0);

                for (let x = 0; x <= W; x += 2) {
                    const p = (x - margin) / graphWidth;
                    const fx = p * maxFx;
                    let freq = 20 * Math.pow(2, fx / 13.0);
                    // clamp freq for math stability outside margins
                    if (freq < 5) freq = 5;
                    if (freq > 24000) freq = 24000;

                    const w0 = fastsincos(freq / fs);
                    const w2 = fastsincos((2.0 * freq) / fs);

                    let mag_sq = 1.0;
                    mag_sq *= biquad_mag_sq(c0, w0, w2);
                    mag_sq *= biquad_mag_sq(c1, w0, w2);
                    mag_sq *= biquad_mag_sq(c2, w0, w2);
                    mag_sq *= biquad_mag_sq(c3, w0, w2);
                    mag_sq *= biquad_mag_sq(c4, w0, w2);

                    let mag = Math.sqrt(mag_sq);
                    if (mag < 0.0001) mag = 0.0001;
                    const db = 20.0 * Math.log10(mag);

                    // map dB to Y [0..H]
                    let y = (H/2) - (db * (H/40)); // +- 20dB range

                    if (x === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                }
                ctx.stroke();

                // Draw interactive nodes
                nodes = [];
                for (let i = 0; i < 5; i++) {
                    const freq = getFloat(i * 2);
                    const db = getFloat(i * 2 + 1);

                    const fx = 13.0 * Math.log2(freq / 20.0);
                    const p = fx / maxFx;
                    const x = margin + p * graphWidth;
                    const y = (H / 2) - (db * (H / 40));

                    nodes.push({x, y});

                    ctx.beginPath();
                    ctx.arc(x, y, 8, 0, 2 * Math.PI);
                    ctx.fillStyle = (typeof activeNodeIdx !== 'undefined' && i === activeNodeIdx) ? '#ffffff' : '#4ecca3';
                    ctx.fill();
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = '#1a1a2e';
                    ctx.stroke();

                    // Draw labels
                    ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
                    ctx.font = '12px "Inter", sans-serif';
                    ctx.textAlign = 'center';
                    let fStr = freq >= 1000 ? (freq/1000).toFixed(2) + 'k' : freq.toFixed(0);
                    ctx.fillText(`${fStr}Hz`, x, y - 24);
                    ctx.fillText(`${db > 0 ? '+' : ''}${db.toFixed(1)}dB`, x, y - 10);
                }
            };



            // Interactive EQ logic
            let isDragging = false;
            let activeNodeIdx = -1;
            let nodes = [];

            let lastEqUpdate = 0;
            // Helper to inverse map freq/db to MIDI val (0-120)
            function updateEqNode(nodeIdx, freq, db) {
                const fIdx = nodeIdx * 2;
                const gIdx = nodeIdx * 2 + 1;
                const fDef = effect.pots[fIdx];
                const gDef = effect.pots[gIdx];

                // Clamp
                freq = Math.max(fDef.min, Math.min(fDef.max, freq));
                db = Math.max(gDef.min, Math.min(gDef.max, db));

                // Inverse freq
                let fVal = 0;
                if (fDef.curve === 'FREQUENCY') {
                    const p = Math.pow((freq - fDef.min) / (fDef.max - fDef.min), 1/3);
                    fVal = Math.round(p * 120.0);
                } else if (fDef.curve === 'LINEAR') {
                    const p = (freq - fDef.min) / (fDef.max - fDef.min);
                    fVal = Math.round(p * 120.0);
                }
                if (isNaN(fVal)) fVal = 0;
                fVal = Math.max(0, Math.min(120, fVal));

                // Inverse gain
                let gVal = 0;
                const gp = (db - gDef.min) / (gDef.max - gDef.min);
                gVal = Math.round(gp * 120.0);
                if (isNaN(gVal)) gVal = 0;
                gVal = Math.max(0, Math.min(120, gVal));

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

            const getMousePos = (e) => {
                const canvas = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                const rect = canvas.getBoundingClientRect();
                const clientX = e.touches ? e.touches[0].clientX : e.clientX;
                const clientY = e.touches ? e.touches[0].clientY : e.clientY;
                const scaleX = canvas.width / rect.width;
                const scaleY = canvas.height / rect.height;
                return {
                    x: (clientX - rect.left) * scaleX,
                    y: (clientY - rect.top) * scaleY
                };
            };

            const onDown = (e) => {
                e.preventDefault();
                const pos = getMousePos(e);
                let minDist = 10000;
                activeNodeIdx = -1;
                nodes.forEach((n, i) => {
                    const dx = n.x - pos.x;
                    const dy = n.y - pos.y;
                    const dist = Math.sqrt(dx*dx + dy*dy);
                    if (dist < 40 && dist < minDist) {
                        minDist = dist;
                        activeNodeIdx = i;
                    }
                });
                if (activeNodeIdx !== -1) {
                    isDragging = true;
                    effect.redrawCurve();
                }
            };

            const onMove = (e) => {
                if (!isDragging || activeNodeIdx === -1) return;
                e.preventDefault();
                const pos = getMousePos(e);
                const canvas = curveWrapper.querySelector(`#eq-canvas-${idx}`);
                const W = canvas.width;
                const H = canvas.height;

                const posX = Math.max(0, Math.min(W, pos.x));
                const posY = Math.max(0, Math.min(H, pos.y));

                const margin = 40;
                const graphWidth = W - 2 * margin;
                const maxFx = 13.0 * Math.log2(20000.0 / 20.0);

                const p = (posX - margin) / graphWidth;
                const fx = p * maxFx;
                const freq = 20.0 * Math.pow(2, fx / 13.0);
                const db = (H / 2 - posY) / (H / 40);

                updateEqNode(activeNodeIdx, freq, db);
            };

            const onUp = (e) => {
                if (isDragging && activeNodeIdx !== -1) {
                    // Force final sysex flush on release
                    const fIdx = activeNodeIdx * 2;
                    const gIdx = activeNodeIdx * 2 + 1;
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
                    canvasEl.addEventListener('mousedown', onDown);
                    canvasEl.addEventListener('mousemove', onMove);
                    canvasEl.addEventListener('mouseup', onUp);
                    canvasEl.addEventListener('mouseleave', onUp);
                    canvasEl.addEventListener('touchstart', onDown, {passive: false});
                    canvasEl.addEventListener('touchmove', onMove, {passive: false});
                    canvasEl.addEventListener('touchend', onUp);
                }
            }, 0);

        } else {
            controls.className = 'effect-controls';
        }

        // Generate Mix slider
        if (effect.name !== 'Settings' && effect.name !== 'Noise Gate') {
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

            ccToElementMap.set(`eff-${idx}-mix`, mixInput);
            mixInput.addEventListener('input', (e) => {
                const midiVal = parseInt(e.target.value);
                mixValDisplay.textContent = formatPotValue(mixPotDef, midiVal);
                sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, 0, midiVal]);
            });

            mixDiv.appendChild(mixLabel);
            mixDiv.appendChild(mixValDisplay);
            mixDiv.appendChild(mixInput);

            // Add active pot triggers
            const activateMixPot = () => {
                setActivePot(`eff-${idx}-mix`, mixPotDef, parseInt(mixInput.value), effect.name);
            };
            mixDiv.addEventListener('mousedown', activateMixPot);
            mixDiv.addEventListener('touchstart', activateMixPot, { passive: true });

            controls.appendChild(mixDiv);
        }

        effect.pots.forEach((pot, pIdx) => {
            const potIdKey = `eff-${idx}-pot-${pIdx}`;

            const potDiv = document.createElement('div');
            potDiv.className = effect.name === 'Parametric EQ' ? 'pot-control eq-pot' : 'pot-control';

            const label = document.createElement('div');
            label.className = 'pot-label';
            label.textContent = pot.name;

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

                if (effect.name === 'Parametric EQ') {
                    input.className = 'eq-range';
                    input.redrawCurve = effect.redrawCurve;
                    eqPotsInputs.push(input);
                }

                ccToElementMap.set(potIdKey, input);
                input.addEventListener('input', (e) => {
                    const midiVal = parseInt(e.target.value);
                    valDisplay.textContent = formatPotValue(pot, midiVal);
                    sendSysex([SYSEX_CMD.PARAM_UPDATE, effect.id, pIdx+1, midiVal]);
                    if (input.redrawCurve) input.redrawCurve();
                });

                if (effect.name === 'Parametric EQ') {
                    const sliderWrapper = document.createElement('div');
                    sliderWrapper.className = 'eq-slider-wrapper';
                    sliderWrapper.appendChild(input);

                    potDiv.appendChild(valDisplay);
                    potDiv.appendChild(sliderWrapper);
                    potDiv.appendChild(label);
                } else {
                    potDiv.appendChild(label);
                    potDiv.appendChild(valDisplay);
                    potDiv.appendChild(input);
                }

                // Add active pot triggers
                const activatePot = () => {
                    setActivePot(potIdKey, pot, parseInt(input.value), effect.name);
                };

                // Allow interaction with the pot div to open the active pot panel
                potDiv.addEventListener('mousedown', activatePot);
                potDiv.addEventListener('touchstart', activatePot, { passive: true });
            }

            if (effect.name === 'Parametric EQ') {
                slidersContainer.appendChild(potDiv);
            } else {
                controls.appendChild(potDiv);
            }
        });

        if (effect.name === 'Parametric EQ') {
            setTimeout(() => effect.redrawCurve(), 0);
        }
        card.appendChild(controls);
        effectsContainer.appendChild(card);
    });
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

    function closeAllPanels() {
        if (document.getElementById('panel-backdrop')) document.getElementById('panel-backdrop').classList.add('hidden');
        if (document.getElementById('global-menu-panel')) document.getElementById('global-menu-panel').classList.add('hidden');
        if (document.getElementById('active-pot-panel')) document.getElementById('active-pot-panel').classList.add('hidden');
        activePotCc = null;
        activePotDef = null;
    }

    const backdrop = document.getElementById('panel-backdrop');
    if (backdrop) {
        backdrop.addEventListener('click', closeAllPanels);
    }

    function showButtonSuccess(btn, successText) {
        const originalText = btn.innerHTML;
        btn.innerHTML = `✓ ${successText}`;
        btn.classList.add('success');
        setTimeout(() => {
            btn.innerHTML = originalText;
            btn.classList.remove('success');
        }, 1500);
    }

    function showButtonError(btn, errorText) {
        const originalText = btn.innerHTML;
        btn.innerHTML = `⚠️ ${errorText}`;
        btn.classList.add('error');
        setTimeout(() => {
            btn.innerHTML = originalText;
            btn.classList.remove('error');
        }, 1500);
    }

    // Global Menu Panel
    const burgerBtn = document.getElementById('burger-btn');
    const closeGlobalMenuBtn = document.getElementById('close-global-menu');
    const globalMenuPanel = document.getElementById('global-menu-panel');

    if (burgerBtn) {
        burgerBtn.addEventListener('click', () => {
            if (globalMenuPanel.classList.contains('hidden')) {
                closeAllPanels();
                globalMenuPanel.classList.remove('hidden');
                if (backdrop) backdrop.classList.remove('hidden');
            } else {
                closeAllPanels();
            }
        });
    }

    if (closeGlobalMenuBtn) {
        closeGlobalMenuBtn.addEventListener('click', () => {
            closeAllPanels();
        });
    }

    // Active Pot initialization
    const closeActivePotBtn = document.getElementById('close-active-pot');
    if (closeActivePotBtn) {
        closeActivePotBtn.addEventListener('click', () => {
            closeAllPanels();
        });
    }

    const activePotSlider = document.getElementById('active-pot-slider');
    if (activePotSlider) {
        activePotSlider.addEventListener('input', (e) => {
            if (activePotCc === null || !activePotDef) return;

            const val = parseInt(e.target.value);
            document.getElementById('active-pot-value').textContent = formatPotValue(activePotDef, val);

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
        closeAllPanels();
        activePotCc = cc;
        activePotDef = potDef;

        document.getElementById('active-pot-name').textContent = `${effectName} - ${potDef.name}`;
        const valDisplay = document.getElementById('active-pot-value');
        if (valDisplay) valDisplay.textContent = formatPotValue(potDef, currentVal);

        if (activePotSlider) activePotSlider.value = currentVal;

        document.getElementById('active-pot-panel').classList.remove('hidden');
        if (backdrop) backdrop.classList.remove('hidden');
    }

    const globalResetBtn = document.getElementById('global-reset-btn');
    if (globalResetBtn) {
        globalResetBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalResetBtn, 'Not Connected');
                return;
            }
            sendMidiCc(GLOBAL_ENABLE_CC, 64);
            setTimeout(() => {
                sendMidiCc(GLOBAL_ENABLE_CC, 127);
                sendSysex([SYSEX_CMD.REQ_SCHEMA]);
                sendSysex([SYSEX_CMD.DIAGNOSTIC]); // Check status after reset
            }, 100);
            showButtonSuccess(globalResetBtn, 'Reset Complete');
        });
    }

    const globalDisableBtn = document.getElementById('global-disable-btn');
    if (globalDisableBtn) {
        globalDisableBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalDisableBtn, 'Not Connected');
                return;
            }

            sendMidiCc(GLOBAL_ENABLE_CC, 67);
            showButtonSuccess(globalDisableBtn, 'Effects Disabled');
        });
    }

    const sceneSelect = document.getElementById('global-scene-select');
    if (sceneSelect) {
        for (let i = 0; i < 32; i++) {
            const opt = document.createElement('option');
            opt.value = i;
            opt.textContent = `Scene ${i}`;
            sceneSelect.appendChild(opt);
        }
    }

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
            showButtonSuccess(loadSceneBtn, 'Loaded!');
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
            showButtonSuccess(saveSceneBtn, 'Saved!');
        });
    }

    const globalProgramBtn = document.getElementById('global-program-btn');
    if (globalProgramBtn) {
        globalProgramBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalProgramBtn, 'Not Connected');
                return;
            }
            if (confirm("Reboot pedal into programming mode?")) {
                sendMidiCc(GLOBAL_ENABLE_CC, 126);
                closeAllPanels();
            }
        });
    }


// Boot
renderUI();
initMidi();
