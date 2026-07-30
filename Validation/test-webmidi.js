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

const WEB = path.join(__dirname, '..', 'Software', 'WebMIDI');
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
        querySelector() { return element('?'); },
        querySelectorAll() { return []; },
        closest() { return null; },
        scrollIntoView() {},
        focus() {},
        getContext() { return new Proxy({}, { get: () => () => {} }); },
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
              'routeEffect', 'unrouteEffect'];

//
// The chain is a list held in a variable rather than anything the dom
// can be asked about, so reading it takes an accessor evaluated in the
// app's own scope.  A snapshot would not do: currentRouting is replaced
// on every routing change, not mutated.
//
const src = fs.readFileSync(effectsJs, 'utf8') + '\n'
          + fs.readFileSync(path.join(WEB, 'app.js'), 'utf8') + '\n'
          + `;globalThis.__app = { ${WANT.join(', ')} };`
          + `;globalThis.__app.routing = () => currentRouting;`;

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
const fallback = path.join(__dirname, '..', 'Software', 'build', 'midi_schema.h');
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
const schema = JSON.parse(schemaJson);
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
const meters = document.getElementById('signal-chain-meters');
const frame = (...body) => [0xF0, 0x7D, 0x0B, ...body, 0xF7];

app.handleTelemetry(frame(1, 38, 61, 18, 127, 42));
check('a full frame reads out', /in −38 dB/.test(meters.textContent)
      && /floor −61 dB/.test(meters.textContent)
      && /out −18 dB/.test(meters.textContent), meters.textContent);
check('an open gate says so', /gate open/.test(meters.textContent),
      meters.textContent);
check('cpu is a percentage', /cpu 33%/.test(meters.textContent),
      meters.textContent);

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

// Newer firmware: extra fields at the end, which must simply be ignored
app.handleTelemetry(frame(1, 12, 34, 56, 127, 64, 99, 98, 97));
check('a long frame is read as far as we understand it',
      /in −12 dB/.test(meters.textContent) && /cpu 50%/.test(meters.textContent),
      meters.textContent);

// A frame with nothing in it at all must not throw or print rubbish
app.handleTelemetry(frame());

if (failures) {
    say(`test-webmidi: ${failures} failure(s)`);
    process.exit(1);
}
say('test-webmidi: app loads; identity, scenes and telemetry behave');
