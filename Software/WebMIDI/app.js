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
        enableGroup.className = 'control-group';
        enableGroup.innerHTML = `
            <label class="switch">
              <input type="checkbox" id="enable-${idx}">
              <span class="slider round"></span>
            </label>
        `;

        header.appendChild(title);
        header.appendChild(enableGroup);
        card.appendChild(header);

        const enableInput = enableGroup.querySelector('input');
        ccToElementMap.set(effect.enable_cc, enableInput);
        enableInput.addEventListener('change', (e) => {
            sendMidiCc(effect.enable_cc, e.target.checked ? 127 : 0);
        });

        // Controls
        const controls = document.createElement('div');
        controls.className = 'effect-controls';

        effect.pots.forEach((pot, pIdx) => {
            const potCc = pot.cc;

            const potDiv = document.createElement('div');
            potDiv.className = 'pot-control';

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

                ccToElementMap.set(potCc, input);
                input.addEventListener('input', (e) => {
                    const midiVal = parseInt(e.target.value);
                    valDisplay.textContent = formatPotValue(pot, midiVal);
                    sendMidiCc(potCc, midiVal);
                });

                potDiv.appendChild(label);
                potDiv.appendChild(valDisplay);
                potDiv.appendChild(input);
            }
            controls.appendChild(potDiv);
        });

        card.appendChild(controls);
        effectsContainer.appendChild(card);
    });
}

// Boot
renderUI();
initMidi();
