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
        appendChild(c) { el.children.push(c); return c; },
        insertBefore(c) { el.children.push(c); return c; },
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
              'renderAttention'];

const src = fs.readFileSync(effectsJs, 'utf8') + '\n'
          + fs.readFileSync(path.join(WEB, 'app.js'), 'utf8') + '\n'
          + `;globalThis.__app = { ${WANT.join(', ')} };`;

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

if (failures) {
    say(`test-webmidi: ${failures} failure(s)`);
    process.exit(1);
}
say('test-webmidi: app loads, identity and scene handling behave');
