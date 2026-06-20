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
        if (!navigator.requestMIDIAccess) {
            appTitleEl.textContent = "Browser Not Supported";
            console.error("Web MIDI API is not supported in this browser.");
            return;
        }
        midiAccess = await navigator.requestMIDIAccess({ sysex: false });
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
        sendMidiCc(STATE_DUMP_CC, 127);
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

function handleMidiMessage(event) {
    const [status, data1, data2] = event.data;

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

function sendMidiCc(cc, val) {
    if (!midiOutput) return;
    midiOutput.send([activeTransmitChannel, cc, val]);

    if (cc === 107) {
        activeTransmitChannel = (val === 0) ? 0xB0 : (0xB0 | ((val - 1) & 0x0F));
    }
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

function renderUI() {
    effectsContainer.innerHTML = '';

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
        if (document.getElementById('active-effect-panel')) document.getElementById('active-effect-panel').classList.add('hidden');
        if (document.getElementById('active-pot-panel')) document.getElementById('active-pot-panel').classList.add('hidden');
        activeEffectIdx = null;
        activeEffectDef = null;
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

    // Active Effect Panel
    const closeActiveEffectBtn = document.getElementById('close-active-effect');
    const activeEffectPanel = document.getElementById('active-effect-panel');
    let activeEffectIdx = null;
    let activeEffectDef = null;

    if (closeActiveEffectBtn) {
        closeActiveEffectBtn.addEventListener('click', () => {
            closeAllPanels();
        });
    }

    const effectResetBtn = document.getElementById('effect-reset-btn');
    if (effectResetBtn) {
        effectResetBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(effectResetBtn, 'Not Connected');
                return;
            }
            if (activeEffectDef) {
                sendMidiCc(activeEffectDef.enable_cc, 64);
                setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
                showButtonSuccess(effectResetBtn, 'Reset Complete');
            }
        });
    }

    const effectSaveBtn = document.getElementById('effect-save-btn');
    if (effectSaveBtn) {
        effectSaveBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(effectSaveBtn, 'Not Connected');
                return;
            }
            if (activeEffectDef) {
                sendMidiCc(activeEffectDef.enable_cc, 65);
                showButtonSuccess(effectSaveBtn, 'Saved!');
            }
        });
    }

    const effectLoadBtn = document.getElementById('effect-load-btn');
    if (effectLoadBtn) {
        effectLoadBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(effectLoadBtn, 'Not Connected');
                return;
            }
            if (activeEffectDef) {
                sendMidiCc(activeEffectDef.enable_cc, 66);
                setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
                showButtonSuccess(effectLoadBtn, 'Loaded!');
            }
        });
    }

    function openActiveEffectPanel(effectIdx) {
        const activeEffectPanel = document.getElementById('active-effect-panel');
        if (activeEffectPanel) {
            closeAllPanels();
            activeEffectIdx = effectIdx;
            activeEffectDef = PEDAL_EFFECTS[effectIdx];

            document.getElementById('active-effect-title').textContent = activeEffectDef.name;
            activeEffectPanel.classList.remove('hidden');
            if (backdrop) backdrop.classList.remove('hidden');
        }
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

            sendMidiCc(activePotCc, val);
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
                sendMidiCc(STATE_DUMP_CC, 127);
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
            setTimeout(() => {
                sendMidiCc(STATE_DUMP_CC, 127);
            }, 100);
            showButtonSuccess(globalDisableBtn, 'Effects Disabled');
        });
    }

    const globalSaveBtn = document.getElementById('global-save-btn');
    if (globalSaveBtn) {
        globalSaveBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalSaveBtn, 'Not Connected');
                return;
            }
            sendMidiCc(GLOBAL_ENABLE_CC, 65);
            showButtonSuccess(globalSaveBtn, 'Saved!');
        });
    }
    const globalLoadBtn = document.getElementById('global-load-btn');
    if (globalLoadBtn) {
        globalLoadBtn.addEventListener('click', () => {
            if (!midiOutput) {
                showButtonError(globalLoadBtn, 'Not Connected');
                return;
            }
            sendMidiCc(GLOBAL_ENABLE_CC, 66);
            setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
            showButtonSuccess(globalLoadBtn, 'Loaded!');
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

    PEDAL_EFFECTS.forEach((effect, idx) => {
        const card = document.createElement('section');
        card.className = 'glass-panel effect-card';
        card.id = `effect-${idx}`;

        // Header
        const header = document.createElement('div');
        header.className = 'effect-header';

        const title = document.createElement('div');
        title.className = 'effect-title';
        title.textContent = effect.name;

        const enableGroup = document.createElement('div');
        enableGroup.className = 'control-group enable-group';
        enableGroup.innerHTML = `
            <label class="switch">
              <input type="checkbox" id="enable-${idx}">
              <span class="slider round"></span>
            </label>
        `;

        header.appendChild(title);
        header.appendChild(enableGroup);
        card.appendChild(header);

        // Clicking the header (but not the toggle) opens the active effect panel
        header.addEventListener('click', (e) => {
            if (e.target.closest('.switch')) return;
            openActiveEffectPanel(idx, effect);
        });

        const enableInput = enableGroup.querySelector('input');
        ccToElementMap.set(effect.enable_cc, enableInput);
        enableInput.addEventListener('change', (e) => {
            sendMidiCc(effect.enable_cc, e.target.checked ? 127 : 0);
        });

        // Controls
        const controls = document.createElement('div');

        let slidersContainer = null;
        let eqPotsInputs = [];

        if (effect.id === 'eq') {
            controls.className = 'effect-controls eq-container';

            const curveWrapper = document.createElement('div');
            curveWrapper.className = 'eq-curve-wrapper';
            curveWrapper.innerHTML = `
                <svg class="eq-curve-svg" viewBox="0 0 1000 100" preserveAspectRatio="none">
                    <path id="eq-path-${idx}" class="eq-path" d="" />
                </svg>
            `;
            controls.appendChild(curveWrapper);

            slidersContainer = document.createElement('div');
            slidersContainer.className = 'eq-sliders';
            controls.appendChild(slidersContainer);

            effect.redrawCurve = () => {
                const pathEl = curveWrapper.querySelector(`#eq-path-${idx}`);
                if (!pathEl || eqPotsInputs.length < 10) return;

                const getY = (val) => 100 - (val / 120) * 100;
                let path = `M 0,${getY(parseInt(eqPotsInputs[0].value))} L 50,${getY(parseInt(eqPotsInputs[0].value))} `;
                for (let i = 0; i < 9; i++) {
                    let x0 = 50 + i * 100;
                    let y0 = getY(parseInt(eqPotsInputs[i].value));
                    let x1 = 50 + (i + 1) * 100;
                    let y1 = getY(parseInt(eqPotsInputs[i+1].value));
                    let mx = (x0 + x1) / 2;
                    path += `C ${mx},${y0} ${mx},${y1} ${x1},${y1} `;
                }
                path += `L 1000,${getY(parseInt(eqPotsInputs[9].value))} `;
                path += `L 1000,100 L 0,100 Z`;
                pathEl.setAttribute('d', path);
            };
        } else {
            controls.className = 'effect-controls';
        }

        effect.pots.forEach((pot, pIdx) => {
            const potCc = pot.cc;

            const potDiv = document.createElement('div');
            potDiv.className = effect.id === 'eq' ? 'pot-control eq-pot' : 'pot-control';

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

                ccToElementMap.set(potCc, select);
                select.addEventListener('change', (e) => {
                    const midiVal = parseInt(e.target.value);
                    sendMidiCc(potCc, midiVal);
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

                if (effect.id === 'eq') {
                    input.className = 'eq-range';
                    input.redrawCurve = effect.redrawCurve;
                    eqPotsInputs.push(input);
                }

                ccToElementMap.set(potCc, input);
                input.addEventListener('input', (e) => {
                    const midiVal = parseInt(e.target.value);
                    valDisplay.textContent = formatPotValue(pot, midiVal);
                    sendMidiCc(potCc, midiVal);
                    if (input.redrawCurve) input.redrawCurve();
                });

                if (effect.id === 'eq') {
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
                    setActivePot(potCc, pot, parseInt(input.value), effect.name);
                };

                // Allow interaction with the pot div to open the active pot panel
                potDiv.addEventListener('mousedown', activatePot);
                potDiv.addEventListener('touchstart', activatePot, { passive: true });
            }

            if (effect.id === 'eq') {
                slidersContainer.appendChild(potDiv);
            } else {
                controls.appendChild(potDiv);
            }
        });

        if (effect.id === 'eq') {
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

// Boot
renderUI();
initMidi();
