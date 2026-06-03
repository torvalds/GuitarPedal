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
        appTitleEl.className = "title-connected";

        // Request initial state dump
        sendMidiCc(STATE_DUMP_CC, 127);
    } else {
        midiInput = null;
        midiOutput = null;
        appTitleEl.className = "title-disconnected";
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

    globalEnableEl.addEventListener('change', (e) => {
        sendMidiCc(GLOBAL_ENABLE_CC, e.target.checked ? 127 : 0);
    });

    function closeAllPanels() {
        if (document.getElementById('global-menu-panel')) document.getElementById('global-menu-panel').classList.add('hidden');
        if (document.getElementById('active-effect-panel')) document.getElementById('active-effect-panel').classList.add('hidden');
        if (document.getElementById('active-pot-panel')) document.getElementById('active-pot-panel').classList.add('hidden');
        activeEffectIdx = null;
        activeEffectDef = null;
        activePotCc = null;
        activePotDef = null;
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
            closeAllPanels();
            globalMenuPanel.classList.remove('hidden');
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

    function openActiveEffectPanel(idx, effect) {
        closeAllPanels();
        activeEffectIdx = idx;
        activeEffectDef = effect;
        document.getElementById('active-effect-title').textContent = effect.name;
        activeEffectPanel.classList.remove('hidden');
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

        document.getElementById('active-pot-title').textContent = `${effectName} - ${potDef.name}`;
        document.getElementById('active-pot-value').textContent = formatPotValue(potDef, currentVal);

        if (activePotSlider) activePotSlider.value = currentVal;

        document.getElementById('active-pot-panel').classList.remove('hidden');
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
