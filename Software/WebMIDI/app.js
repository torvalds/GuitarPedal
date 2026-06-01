let midiAccess = null;
let midiInput = null;
let midiOutput = null;

// UI Elements
const statusEl = document.getElementById('connection-status');
const globalEnableEl = document.getElementById('global-enable');
const effectsContainer = document.getElementById('effects-container');

// Map of CC to its HTML input element for O(1) updates
const ccToElementMap = new Map();

// State
let isGlobalEnabled = false;

// Initialize MIDI
async function initMidi() {
    try {
        midiAccess = await navigator.requestMIDIAccess({ sysex: false });
        midiAccess.onstatechange = updateMidiState;
        updateMidiState();
    } catch (err) {
        console.error("MIDI access denied", err);
        statusEl.textContent = "MIDI Error";
    }
}

function updateMidiState() {
    let foundInput = null;
    let foundOutput = null;

    for (let input of midiAccess.inputs.values()) {
        if (input.name.includes('Linus Pedal')) {
            foundInput = input;
            break;
        }
    }

    for (let output of midiAccess.outputs.values()) {
        if (output.name.includes('Linus Pedal')) {
            foundOutput = output;
            break;
        }
    }

    if (foundInput && foundOutput) {
        if (midiInput !== foundInput) {
            midiInput = foundInput;
            midiInput.onmidimessage = handleMidiMessage;
        }
        midiOutput = foundOutput;
        statusEl.textContent = "Connected";
        statusEl.className = "status connected";

        // Request initial state dump
        sendMidiCc(STATE_DUMP_CC, 127);
    } else {
        midiInput = null;
        midiOutput = null;
        statusEl.textContent = "Disconnected";
        statusEl.className = "status disconnected";
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
            isGlobalEnabled = (val > 0);
            globalEnableEl.checked = isGlobalEnabled;
        } else {
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
    }
}

function sendMidiCc(cc, val) {
    if (!midiOutput) return;
    midiOutput.send([0xB0, cc, val]);
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
    }

    let val = Math.round(p * 120);
    return Math.max(0, Math.min(120, val));
}

function renderUI() {
    effectsContainer.innerHTML = '';

    globalEnableEl.addEventListener('change', (e) => {
        sendMidiCc(GLOBAL_ENABLE_CC, e.target.checked ? 127 : 0);
    });

    const globalResetBtn = document.getElementById('global-reset-btn');
    if (globalResetBtn) {
        globalResetBtn.addEventListener('click', () => {
            sendMidiCc(GLOBAL_ENABLE_CC, 64);
            setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
        });
    }
    const globalSaveBtn = document.getElementById('global-save-btn');
    if (globalSaveBtn) {
        globalSaveBtn.addEventListener('click', () => sendMidiCc(GLOBAL_ENABLE_CC, 65));
    }
    const globalLoadBtn = document.getElementById('global-load-btn');
    if (globalLoadBtn) {
        globalLoadBtn.addEventListener('click', () => {
            sendMidiCc(GLOBAL_ENABLE_CC, 66);
            setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
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
            <button class="action-btn" id="reset-btn-${idx}" title="Reset to Defaults">↺</button>
            <button class="action-btn" id="save-btn-${idx}" title="Save to EEPROM">💾</button>
            <button class="action-btn" id="load-btn-${idx}" title="Load from EEPROM">📂</button>
            <label class="switch">
              <input type="checkbox" id="enable-${idx}">
              <span class="slider round"></span>
            </label>
        `;

        header.appendChild(title);
        header.appendChild(enableGroup);
        card.appendChild(header);

        const resetBtn = enableGroup.querySelector(`#reset-btn-${idx}`);
        resetBtn.addEventListener('click', () => {
            sendMidiCc(effect.enable_cc, 64);
            setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
        });

        const saveBtn = enableGroup.querySelector(`#save-btn-${idx}`);
        saveBtn.addEventListener('click', () => {
            sendMidiCc(effect.enable_cc, 65);
        });

        const loadBtn = enableGroup.querySelector(`#load-btn-${idx}`);
        loadBtn.addEventListener('click', () => {
            sendMidiCc(effect.enable_cc, 66);
            setTimeout(() => sendMidiCc(STATE_DUMP_CC, 127), 100);
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

// Boot
renderUI();
initMidi();
