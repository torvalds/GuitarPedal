//
// Does the web app still load, and does it still draw the right
// conclusions?
//
// The firmware has a compiler and two post-build checks looking over its
// shoulder.  The app has none of that, and 'node --check' is not a
// substitute: it parses, and a parser will happily accept an identifier
// that was never declared.  That is exactly the mistake that gets made
// when a block is deleted and something further down still wanted a name
// it happened to define - which is a runtime ReferenceError in the
// browser and silence everywhere else.
//
// So load it for real, against enough of a DOM to get through its boot
// path, and then call the handful of functions whose logic decides what
// the user is told.
//
// getElementById() is deliberately strict: it knows the ids in
// index.html, plus any the app assigns as it builds cards, and returns
// null for anything else.  So asking for an element that no longer exists
// in the markup fails here instead of quietly doing nothing in front of
// somebody.
//
// Called as: node test-webmidi.js <generated effects.js>
//
'use strict';

const fs = require('fs');
const path = require('path');

const WEB = path.join(__dirname, '..', 'WebMIDI');
const effectsJs = process.argv[2] || path.join(WEB, 'effects.js');

let failures = 0;

const say = (line) => process.stdout.write(line + '\n');

function check(what, ok, detail) {
    if (ok) return;
    say(`  FAIL: ${what}${detail ? ' - ' + detail : ''}`);
    failures++;
}

//
// A DOM, to the depth this actually needs.
//
const byId = new Map();

function element(id) {
    const classes = new Set();
    const el = {
        children: [],
        parentElement: null,
        style: {},
        dataset: {},
        textContent: '',
        value: '',
        title: '',
        checked: false,
        classes,
        classList: {
            add: (c) => classes.add(c),
            remove: (c) => classes.delete(c),
            contains: (c) => classes.has(c),
            toggle: (c, on) => (on === undefined
                ? (classes.has(c) ? classes.delete(c) : classes.add(c))
                : (on ? classes.add(c) : classes.delete(c))),
        },
        get id() { return this._id; },
        set id(v) { this._id = v; byId.set(v, this); },
        get innerHTML() { return ''; },
        set innerHTML(v) { if (v === '') el.children.length = 0; },
        // Parentage is tracked because the app walks upwards as well as
        // down - a pot's value display is found from the input via its
        // parent, so an element that does not know its parent makes
        // resetUnroutedEffects() throw rather than do nothing.
        appendChild(c) { el.children.push(c); c.parentElement = el; return c; },
        insertBefore(c) { el.children.push(c); c.parentElement = el; return c; },
        removeChild(c) { return c; },
        remove() {},
        addEventListener() {},
        removeEventListener() {},
        dispatchEvent() { return true; },
        setAttribute() {},
        getAttribute() { return null; },
        // The same selector has to come back as the same node, or a
        // canvas fetched twice is two different canvases and nothing
        // drawn on it can be observed.
        querySelector(sel) {
            if (!el._q) el._q = new Map();
            if (!el._q.has(sel)) el._q.set(sel, element('?'));
            return el._q.get(sel);
        },
        querySelectorAll() { return []; },
        closest() { return null; },
        // Nothing here is laid out, so everything has no size - which is
        // also what a real parked card reports, and is the case the
        // drawing code has a fallback for.
        getBoundingClientRect() {
            return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 };
        },
        scrollIntoView() {},
        focus() {},
        // A canvas context that remembers what it was asked to do,
        // so a test can tell "drew the wrong thing" from "drew nothing"
        getContext() {
            if (!el._ctx) {
                const calls = [];
                el._ctx = new Proxy({}, {
                    get: (t, k) => (k === 'calls' ? calls : () => calls.push(k)),
                    set: () => true,
                });
            }
            return el._ctx;
        },
    };
    el._id = id;
    return el;
}

// Everything index.html declares is real; everything else is not.
for (const m of fs.readFileSync(path.join(WEB, 'index.html'), 'utf8')
                  .matchAll(/\bid="([^"]+)"/g))
    byId.set(m[1], element(m[1]));

global.document = {
    getElementById: (id) => byId.get(id) || null,
    createElement: () => element(undefined),
    // Enough of a node to be appended and to carry its text
    createTextNode: (text) => ({ textContent: text, parentElement: null }),
    querySelector: () => element('?'),
    querySelectorAll: () => [],
    addEventListener() {},
    body: element('body'),
    head: element('head'),
    documentElement: element('html'),
};
global.window = {
    addEventListener() {},
    matchMedia: () => ({ matches: false, addEventListener() {} }),
    location: { hostname: 'localhost', href: 'http://localhost/' },
};
global.location = global.window.location;
// Node has a real navigator of its own, and it is read-only, so this has
// to be defined over the top rather than assigned.
//
// MIDI access succeeds with nothing plugged in, which is the state the
// app starts in anyway and exercises its device enumeration rather than
// its failure path.
Object.defineProperty(global, 'navigator', {
    value: {
        requestMIDIAccess: () => Promise.resolve({
            inputs: new Map(), outputs: new Map(),
            onstatechange: null, addEventListener() {},
        }),
    },
    configurable: true,
    writable: true,
});
global.Event = class { constructor(type) { this.type = type; } };
global.alert = () => {};
global.confirm = () => true;
global.localStorage = { getItem: () => null, setItem() {}, removeItem() {} };

//
// Load it.  The app is a plain script, so its declarations are scoped to
// the function we build here - the trailing export is how the tests below
// get at the ones they need.
//
const WANT = ['handleIdentity', 'populateScenePicker', 'updateSceneLabels',
              'boardFault', 'earlyNote', 'clipFault', 'dropFault',
              'renderAttention', 'handleTelemetry', 'handleSysex',
              'routeEffect', 'unrouteEffect',
              'potToValue', 'valueToPot', 'clampToNeighbours', 'pileAt',
              'uiPref', 'setUiPref',
              'controlDef', 'actionsFor'];

//
// The chain is a list held in a variable rather than anything the dom
// can be asked about, so reading it takes an accessor evaluated in the
// app's own scope.  A snapshot would not do: currentRouting is replaced
// on every routing change, not mutated.
//
const src = fs.readFileSync(effectsJs, 'utf8') + '\n'
          + fs.readFileSync(path.join(WEB, 'app.js'), 'utf8') + '\n'
          + `;globalThis.__app = { ${WANT.join(', ')} };`
          + `;globalThis.__app.routing = () => currentRouting;`
          + `;globalThis.__app.controls = () => CONTROLS;`;

//
// The app logs as it goes, including from promises that settle after this
// script has finished, so console stays silenced for the whole run and
// anything worth saying goes out directly.
//
for (const k of ['log', 'debug', 'info', 'warn', 'error'])
    console[k] = () => {};

try {
    new Function(src)();
} catch (e) {
    say(`  FAIL: app.js did not load - ${e.name}: ${e.message}`);
    const at = (e.stack || '').split('\n')[1];
    if (at) say(`        ${at.trim()}`);
    process.exit(1);
}

const app = globalThis.__app;
for (const name of WANT)
    check(`${name} is defined`, app[name] !== undefined);

//
// The identity reply decides whether the user gets told their board is
// wrong, and it is the thing most likely to be quietly inverted.
//
const identity = (over) => Object.assign({
    build: 'Jan 1 2026 00:00:00', scenes: 32, midi_hw: true,
    found: { eeprom: true, legacy_codec: false, legacy_screen: false },
}, over);

app.handleIdentity(identity());
check('a current board raises no warning', app.boardFault.on === false);

app.handleIdentity(identity({ scenes: 1, found: { eeprom: true, legacy_codec: true } }));
check('an early board is noted, not faulted', app.earlyNote.on === true
      && app.boardFault.on === false);
check('and says why', /TAC5112/.test(document.getElementById('status-early').title),
      document.getElementById('status-early').title);

app.handleIdentity(identity({ found: { eeprom: false } }));
check('a missing eeprom is flagged', app.boardFault.on === true);

app.handleIdentity(identity());
check('and both clear again when the next pedal is fine',
      app.boardFault.on === false && app.earlyNote.on === false);

//
// The controls come from the pedal, so a board with a different set of
// them needs no change here.  The ids are not positions: a board without
// an expression jack sends five and they are not numbered 0 to 4.
//
app.handleIdentity(identity({ controls: [
    { id: 0, name: 'Knob \u2014 turn', kind: 'turn' },
    { id: 3, name: 'Footswitch \u2014 press', kind: 'click' },
    { id: 5, name: 'Jack tip \u2014 press', kind: 'click' },
] }));
check('the control list is the pedal\u0027s', app.controls().length === 3);
check('and is found by id rather than by position',
      app.controlDef(5).name === 'Jack tip \u2014 press');

//
// Which actions are worth offering follows the kind the pedal gave.
//
const turnActs = app.actionsFor(0).map(a => a.v);
const clickActs = app.actionsFor(3).map(a => a.v);
check('something that turns can only drive a parameter',
      turnActs.length === 2 && turnActs.includes(1));
check('and something that clicks can do anything else',
      clickActs.length > 2 && !clickActs.includes(1));

//
// A rule can name a control the pedal did not describe - an older app
// against a newer pedal.  Drawing it as something beats throwing.
//
check('an unknown control still draws', app.controlDef(99).name === 'Control 99');

app.handleIdentity(identity());

//
// How many scenes there are comes from the pedal, so the picker has to be
// built from the reply rather than guessed at.
//
const picker = document.getElementById('global-scene-select');

app.populateScenePicker(32);
check('32 scenes gives 32 options', picker.children.length === 32,
      `got ${picker.children.length}`);

app.populateScenePicker(1);
check('one scene gives one option', picker.children.length === 1,
      `got ${picker.children.length}`);

app.handleIdentity(identity({ scenes: 1 }));
check('and the identity reply drives it', picker.children.length === 1,
      `got ${picker.children.length}`);

// Nothing routed and nothing asking: this must not throw on a fresh page
app.renderAttention();

//
// Feed it the pedal's actual schema, which is the only way to get the
// effect cards built - and building them is worth exercising, since it is
// where most of the app's DOM work happens.  midi_schema.h is generated
// next to effects.js, so it is wherever that was found.
//
const schemaPath = path.join(path.dirname(path.resolve(effectsJs)), 'midi_schema.h');
const fallback = path.join(__dirname, '..', 'build', 'midi_schema.h');
const schemaFile = fs.existsSync(schemaPath) ? schemaPath
                 : (fs.existsSync(fallback) ? fallback : null);

if (!schemaFile) {
    say('  FAIL: no midi_schema.h found, so no cards can be built');
    process.exit(1);
}

const literal = fs.readFileSync(schemaFile, 'utf8').match(/"((?:[^"\\]|\\.)*)"/);
const schemaJson = literal[1].replace(/\\"/g, '"');
const asSysex = (cmd, text) => [0xF0, 0x7D, cmd,
                                ...[...text].map((c) => c.charCodeAt(0)), 0xF7];

app.handleSysex(asSysex(0x02, schemaJson));
check('the real schema builds the cards',
      document.getElementById('signal-chain-meters') !== null);

//
// Routing.  The chain is an ordered list of ids and everything else is a
// chip in the pool, so the two questions worth asking are whether the
// pool is exactly the complement of the chain, and whether the two anchor
// effects stay out of both - they are the ends of the chain and cannot be
// routed, unrouted or reordered.
//
//
// The schema is an object with the effects under a key now, and used to
// be a bare array of them.  Accepted both ways here for the same reason
// the app accepts both: this test is run against whatever midi_schema.h
// happens to be lying about, including one built before the change.
//
const parsedSchema = JSON.parse(schemaJson);
const schema = Array.isArray(parsedSchema) ? parsedSchema : parsedSchema.effects;
const steering = Array.isArray(parsedSchema) ? null : parsedSchema.steering;

check('the schema declares channel steering once, not per effect',
      steering !== null && steering.pots.length === 3 &&
      steering.pots.map((p) => p.index).join(',') === '11,12,13');

//
// Steering means nothing for an effect that is never handed to
// do_effect_step(), and those are exactly the two that anchor the chain.
//
check('only the routable effects are steerable',
      schema.filter((e) => e.steerable).length === schema.length - 2 &&
      !schema[0].steerable && !schema[schema.length - 1].steerable);

const routable = schema.slice(1, -1);
const pool = document.getElementById('effect-pool');

// [label, chip grid], or hidden with nothing in it at all
const chipNames = () => (pool.classes.has('hidden') ? []
                         : pool.children[pool.children.length - 1]
                               .children.map((c) => c.textContent));

const cardOf = (effect) =>
      document.getElementById(`effect-${schema.indexOf(effect)}`);

check('nothing is routed until the pedal says so',
      chipNames().join() === routable.map((e) => e.name).join(),
      chipNames().join());
check('and the cards for those are parked',
      routable.every((e) => cardOf(e).classes.has('parked')));
check('while the anchors are not chips and not parked',
      !chipNames().includes(schema[0].name)
      && !cardOf(schema[0]).classes.has('parked')
      && !cardOf(schema[schema.length - 1]).classes.has('parked'));

// Two of them routed, in an order that is not the schema's
const routed = [routable[2], routable[0]];
app.handleSysex([0xF0, 0x7D, 0x08, ...routed.map((e) => e.id), 0xF7]);

check('a routed effect leaves the pool',
      chipNames().join() ===
      routable.filter((e) => !routed.includes(e)).map((e) => e.name).join(),
      chipNames().join());
check('and its card comes back',
      routed.every((e) => !cardOf(e).classes.has('parked')));

// And back out again, which is the same path the eject button takes
app.handleSysex([0xF0, 0x7D, 0x08, 0xF7]);
check('an empty routing order empties the chain',
      chipNames().join() === routable.map((e) => e.name).join(),
      chipNames().join());
check('and parks the cards again',
      routable.every((e) => cardOf(e).classes.has('parked')));

//
// Tapping a chip adds to the end, and that is the contract the pool
// makes: order is yours to set by dragging afterwards, so nothing may
// quietly sort the chain back into schema order.
//
app.routeEffect(routable[5].id);
app.routeEffect(routable[1].id);
check('chips route to the end, in the order they were tapped',
      app.routing().join() === [routable[5].id, routable[1].id].join(),
      app.routing().join());

// Which is what the eject button and the flick both come down to
app.unrouteEffect(routable[5].id);
check('unrouting one leaves the others alone',
      app.routing().join() === String(routable[1].id), app.routing().join());
check('and puts just that one back in the pool',
      chipNames().includes(routable[5].name)
      && !chipNames().includes(routable[1].name));

//
// Telemetry.  The layout is append-only, so the interesting cases are the
// short frame from older firmware and the long one from newer - neither is
// an error, and a missing field is absent rather than zero.
//
// The load is the field that grew: layout 2 puts a 14-bit value across
// bytes 5 and 6, where layout 1 had seven bits in byte 5 alone.  Both
// have to read, and which one arrived is decided by whether byte 6 is
// there rather than by the version, because that is how every other
// field here is handled.
//
const meters = document.getElementById('signal-chain-meters');
const frame = (...body) => [0xF0, 0x7D, 0x0B, ...body, 0xF7];

app.handleTelemetry(frame(1, 38, 61, 18, 127, 42));
check('a full frame reads out', /in −38 dB/.test(meters.textContent)
      && /floor −61 dB/.test(meters.textContent)
      && /out −18 dB/.test(meters.textContent), meters.textContent);
check('an open gate says so', /gate open/.test(meters.textContent),
      meters.textContent);
check('cpu is a percentage', /cpu 33\.1%/.test(meters.textContent),
      meters.textContent);

app.handleTelemetry(frame(2, 40, 60, 20, 127, 42, 65));
check('a 14-bit load reads finer than a whole percent',
      /cpu 33\.2%/.test(meters.textContent), meters.textContent);

app.handleTelemetry(frame(1, 0, 127, 0, 0, 127));
check('silence is not a number', /floor −∞ dB/.test(meters.textContent),
      meters.textContent);
check('a closed gate says so', /gate closed/.test(meters.textContent),
      meters.textContent);
check('full scale reads zero', /in −0 dB/.test(meters.textContent),
      meters.textContent);

app.handleTelemetry(frame(1, 30, 55, 20, 32, 10));
check('a gating gate reads in dB', /gate −12 dB/.test(meters.textContent),
      meters.textContent);

// Older firmware: fields this app knows about are simply not there
app.handleTelemetry(frame(1, 40, 60));
check('a short frame keeps what it has', /in −40 dB/.test(meters.textContent)
      && /floor −60 dB/.test(meters.textContent), meters.textContent);
check('and does not invent the rest', !/out/.test(meters.textContent)
      && !/cpu/.test(meters.textContent), meters.textContent);

// Newer firmware: extra fields at the end, which must simply be ignored.
// Byte 6 is the load's LSB and is understood; 98 and 97 are not.
app.handleTelemetry(frame(2, 12, 34, 56, 127, 64, 99, 98, 97));
check('a long frame is read as far as we understand it',
      /in −12 dB/.test(meters.textContent)
      && /cpu 50\.6%/.test(meters.textContent),
      meters.textContent);

// A frame with nothing in it at all must not throw or print rubbish
app.handleTelemetry(frame());

//
// Pot curves.  Every curve has to survive the round trip, because the
// EQ converts both ways constantly - screen position to pot value on the
// way in, pot value to a frequency to draw on the way out - and a curve
// that does not come back to where it started makes a node creep every
// time it is touched.
//
for (const pot of [{ curve: 'LINEAR', min: -20, max: 20 },
                   { curve: 'LINEAR', min: -40, max: 0 },
                   { curve: 'FREQUENCY', min: 20, max: 400 },
                   { curve: 'SQUARED', min: 0, max: 100 },
                   { curve: 'EXPONENTIAL', min: 20, max: 20000 }]) {
    let bad = 0;
    for (let v = 0; v <= 120; v++)
        if (app.valueToPot(pot, app.potToValue(pot, v)) !== v)
            bad++;
    check(`${pot.curve}(${pot.min} ${pot.max}) round-trips`, bad === 0,
          `${bad} of 121 values`);
}

// And the app has to agree with the generator about where a default sits
let defaultsDiffer = 0;
for (const eff of schema)
    for (const pot of eff.pots)
        if (pot.defaultPot !== undefined
            && app.valueToPot(pot, pot.default) !== pot.defaultPot)
            defaultsDiffer++;
check('defaults agree with gen_effects.py', defaultsDiffer === 0,
      `${defaultsDiffer} pots disagree`);

//
// The EQ's ordering rules.  A band may reach its neighbours and no
// further, and the ends of the chain are bounded by the pot instead.
//
const order = [10, 30, 30, 60, 100];

check('a band stops at the neighbour above',
      app.clampToNeighbours(order, 0, 90) === 30);
check('and at the neighbour below',
      app.clampToNeighbours(order, 3, 5) === 30);
check('an unobstructed band moves where it was asked',
      app.clampToNeighbours(order, 3, 45) === 45);
check('the first band is bounded by the bottom of the pot',
      app.clampToNeighbours(order, 0, -5) === 0);
check('the last by the top of it',
      app.clampToNeighbours(order, 4, 999) === 120);

// Whatever it is asked for, the result never crosses a neighbour
let crossed = 0;
for (let idx = 0; idx < 5; idx++)
    for (let want = -20; want <= 140; want++) {
        const got = app.clampToNeighbours(order, idx, want);
        const next = order.slice();
        next[idx] = got;
        for (let i = 1; i < next.length; i++)
            if (next[i] < next[i - 1]) crossed++;
    }
check('no request of any size can put the bands out of order', crossed === 0,
      `${crossed} orderings broken`);

//
// Which of a pile you get.  Equality is what makes a pile, so bands
// that merely sit close are not one.
//
check('a band on its own is a pile of one',
      app.pileAt(order, 0).join() === '0');
check('bands at the same value are a pile',
      app.pileAt(order, 1).join() === '1,2'
      && app.pileAt(order, 2).join() === '1,2');
check('going left takes the lowest, which is the one free to move',
      app.pileAt(order, 2)[0] === 1);
check('going right takes the highest',
      app.pileAt(order, 1).slice(-1)[0] === 2);

//
// Every effect that declares a graph has to actually draw one.
//
// This is the check that was missing when the curve code carried a
// hardcoded band count: the five-band EQ went on working and a two-band
// effect drew nothing at all, silently, because the guard that waits for
// the pots to arrive was written as "fewer than ten".
//
const canvasOf = (idx) => {
    const card = document.getElementById(`effect-${idx}`);
    const controls = card.children.find(
        (c) => (c.className || '').includes('eq-container'));
    const wrapper = controls.children.find(
        (c) => (c.className || '').includes('eq-curve-wrapper'));
    return wrapper.querySelector(`#eq-canvas-${idx}`);
};

let graphed = 0;
schema.forEach((eff, idx) => {
    if (!eff.graph || !eff.graph.length)
        return;
    graphed++;

    const ctx = canvasOf(idx).getContext('2d');
    const before = ctx.calls.length;

    // Any parameter update redraws the curve
    app.handleSysex([0xF0, 0x7D, 0x03, eff.id, 1, 60, 0xF7]);
    const drew = ctx.calls.slice(before);

    check(`${eff.name} draws its response`, drew.includes('stroke'),
          `${drew.length} drawing calls`);
    check(`${eff.name} draws one node per band`,
          drew.filter((c) => c === 'arc').length === eff.graph.length,
          `${drew.filter((c) => c === 'arc').length} of ${eff.graph.length}`);
});
check('the schema has graphed effects to check', graphed > 0);

// Q has to come from the declaration, or the picture is not the filter
let badQ = 0;
for (const eff of schema)
    for (const band of eff.graph || [])
        if (!(band.q > 0) && band.qPot === undefined) badQ++;
check('every graphed band declares a usable Q', badQ === 0, `${badQ} without one`);

//
// The pedal filters incoming MIDI by one pot, and the app has to
// transmit on the same channel or bypass, the tuner and scene changes
// stop arriving.  Which pot that is comes from the schema now, so the
// schema has to actually say.
//
const channels = schema.filter((e) => e.roles && e.roles.CHANNEL !== undefined);
check('exactly one effect owns the MIDI channel', channels.length === 1,
      `${channels.length} claim it`);
if (channels.length === 1) {
    const eff = channels[0];
    check('and it points at a pot that exists',
          eff.pots[eff.roles.CHANNEL] !== undefined);
    check('with the sixteen channels and omni to choose from',
          (eff.pots[eff.roles.CHANNEL].enum || []).length === 17,
          `${(eff.pots[eff.roles.CHANNEL].enum || []).length} options`);
}

//
// A pot with no node on the graph has to be reachable.  Graphed effects
// hide their sliders because the nodes are the controls, and a pot
// outside the band pairs would otherwise be hidden along with them -
// present, working, and impossible to touch.
//
schema.forEach((eff, idx) => {
    const spare = eff.pots.length - (eff.graph || []).length * 2;
    if (!eff.graph || !eff.graph.length || spare <= 0)
        return;

    const card = document.getElementById(`effect-${idx}`);
    const controls = card.children.find(
        (c) => (c.className || '').includes('eq-container'));
    const hidden = controls.children.find(
        (c) => (c.className || '').includes('eq-sliders'));
    const footer = controls.children.find(
        (c) => (c.className || '').includes('eq-footer'));

    check(`${eff.name} hides only the pots the graph draws`,
          hidden.children.length === eff.graph.length * 2,
          `${hidden.children.length} hidden, ${eff.graph.length * 2} banded`);
    check(`${eff.name} puts its ${spare} spare pot(s) where they can be used`,
          footer.children.filter(
              (c) => (c.className || '').includes('pot-control')).length
          === spare + 1,      // and the Mix control
          `${footer.children.length} in the footer`);
});

//
// UI preferences.  The interesting case is storage that refuses to work
// at all, which is a real browser configuration and must not take the
// app down with it - these are preferences, and the cost of losing one
// is that it goes back to its default.
//
check('an unset preference is its default',
      app.uiPref('nothing.here', true) === true
      && app.uiPref('nothing.here', false) === false);

const store = new Map();
global.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, v),
};
app.setUiPref('eq.keepOrder', false);
check('a preference survives a write and a read',
      app.uiPref('eq.keepOrder', true) === false);
app.setUiPref('eq.keepOrder', true);
check('and back again', app.uiPref('eq.keepOrder', false) === true);

global.localStorage = {
    getItem() { throw new Error('denied'); },
    setItem() { throw new Error('denied'); },
};
check('storage that refuses to read still yields the default',
      app.uiPref('eq.keepOrder', true) === true);
app.setUiPref('eq.keepOrder', false);   // and refusing to write is not fatal

if (failures) {
    say(`test-webmidi: ${failures} failure(s)`);
    process.exit(1);
}
say('test-webmidi: app loads; identity, scenes and telemetry behave');
