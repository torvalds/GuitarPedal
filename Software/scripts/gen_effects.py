#!/usr/bin/env python3
import sys
import os
import re
import hashlib
import json
import math
from collections import Counter

def c_string(s):
    """Escape a string into the body of a C string literal.

    JSON and C both spell an escape with a backslash and disagree about
    the rest, so a JSON document is not a C literal and cannot be turned
    into one by attending to the quotes alone.  This used to be a
    replace() of '"', which met every `\\"` that json.dumps() had already
    written and made it `\\\\"` - an escaped backslash followed by a bare
    quote, ending the literal early and compiling the remaining twelve
    kilobytes of schema as source.

    Octal rather than hex for anything unprintable, because a hex escape
    is greedy: `\\x7` next to a literal '9' is one character `\\x79` and
    not two, while `\\007` stops after three digits whatever follows it.
    """
    out = []
    for ch in s:
        if ch in '\\"':
            out.append('\\' + ch)
        elif ' ' <= ch <= '~':
            out.append(ch)
        else:
            out.append('\\%03o' % ord(ch))
    return ''.join(out)


def short_hash(*parts):
    """A 32-bit tag over a canonical description.

    The first four bytes of a SHA-256.  Overkill for telling whether two
    effects are the same effect, and chosen anyway because it runs once
    per build on a workstation and never on the pedal - what ends up in
    the firmware is a constant.  Free is free, and there is no algorithm
    here to reimplement or get subtly wrong the day something else wants
    to compute the same number.

    The pieces are joined with a byte that cannot appear in any of them,
    so that ["ab", "c"] and ["a", "bc"] are different inputs.
    """
    blob = "\x1f".join(parts).encode("utf-8")
    return int.from_bytes(hashlib.sha256(blob).digest()[:4], "big")


def effect_id_hash(short_name, copy):
    """Which effect this is, for matching saved state against.

    The copy index is in there because copies share a short name -
    tone and tone2 are both [TONE] - and two effects that cannot be told
    apart would load each other's settings.
    """
    return short_hash(short_name, str(copy))


def pot_layout_hash(pots):
    """What this effect's pots mean, so stale values can be spotted.

    Covers what decides how to read a stored 0..120 byte: which pot is
    where, its curve, its range, and for an enumeration the list of what
    the indices stand for.  The label is in there too, on the grounds
    that renaming a control is usually a sign of repurposing it.

    Deliberately absent: the default, and the unit.  Neither changes
    what an already-stored value means, and invalidating every saved
    scene because somebody retuned a default would be a poor trade.
    """
    parts = [str(len(pots))]
    for pot in pots:
        parts += [pot['label'], pot['curve']]
        parts += pot['args']
        parts += pot['enum'] or []
    return short_hash(*parts)


def c_ident(where, what, text):
    """Turn a label into the identifier fragment that names it in C.

    Runs of anything that is not alphanumeric collapse to one underscore
    and the ends are trimmed - so "Bass Freq" is BASS_FREQ, "USB L/R In"
    is USB_L_R_IN, and the two spaces settings.h pads "  ATTN" with for
    the app's benefit simply fall off.

    A label is written to be read by a person, so it can say anything.
    Refuse the ones that cannot be an identifier here rather than
    emitting something that will not compile, because the mistake
    belongs to the header that spelled the label and not to a generated
    file nobody meant to read.
    """
    ident = re.sub(r'[^0-9A-Za-z]+', '_', text).strip('_')
    if not ident:
        raise SystemExit(f"{where}: {what} '{text}' has nothing in it that "
                         f"can be part of a C identifier")
    if ident[0].isdigit():
        raise SystemExit(f"{where}: {what} '{text}' starts with a digit, so "
                         f"'{ident}' cannot be a C identifier")
    return ident


def default_pot_value(pot):
    """Turn a pot default in engineering units into the raw 0..120 value.

    This is deliberately the *only* place that conversion happens.  The
    same number goes into the C table and into the schema the web app
    reads, so the two can't drift apart - which they did, back when the
    app recomputed it for itself."""
    y = pot['default']
    curve = pot['curve']

    if curve in ('RAW', 'ENUM'):
        return int(round(y))

    a, b = 0.0, 1.0
    if len(pot['args']) >= 2:
        a = float(pot['args'][0])
        b = float(pot['args'][1])

    if b == a:
        p = 0.0
    else:
        # A default outside the declared range is a mistake in the effect
        # header, but don't take a fractional root of a negative number
        # over it - python hands back a complex and the build dies
        # somewhere confusing.
        ratio = max(0.0, (y - a) / (b - a))
        if curve == 'LINEAR':
            p = ratio
        elif curve == 'FREQUENCY':
            p = ratio ** (1/3.0)
        elif curve == 'SQUARED':
            p = ratio ** 0.5
        elif curve == 'EXPONENTIAL':
            p = math.log2(y / a) / math.log2(b / a) if (a != 0 and y != 0) else 0.0
        else:
            p = 0.0

    return max(0, min(120, int(round(p * 120))))


def generate(audio_dir, out_h, out_js, out_md):
    ui_effects = [] # List of dicts for JS/Schema output
    sources = []    # one per effects/*.h - what gets emitted once
    effects_data = []   # one per copy - what gets an id and a struct

    for filename in os.listdir(audio_dir):
        if not filename.endswith('.h'):
            continue
        base = filename[:-2]
        header_path = os.path.join(audio_dir, filename)

        with open(header_path, 'r') as f:
            content = f.read()

        name_match = re.search(r'//\s*NAME:\s*(.*?)\s*\[(.*?)\]', content)
        priority_match = re.search(r'//\s*PRIORITY:\s*(\d+)', content)
        if not name_match:
            continue

        # effect_id will be assigned later based on sorted index
        effect_id = 0

        full_name = name_match.group(1).strip()
        short_name = name_match.group(2).strip()
        priority = int(priority_match.group(1)) if priority_match else 100

        #
        # 'COPIES: 2' asks for two of this effect rather than one.
        #
        # An effect owns one set of state, so routing the same one twice
        # would run a filter through its own delay line - which is why
        # there have to be two of a thing you might want twice.  This
        # used to be done with a symlink, one file under two names, and
        # the duplication was invisible from inside the file: nothing in
        # tone.h said there were two of it, and the second name existed
        # only in a directory listing.
        #
        # The copies differ in exactly one way, which is that each has
        # its own state.  So everything derived from the POT: lines -
        # the constants, the accessors, the Q table - is emitted once and
        # shared, and only _state, _init and _step are generated per
        # copy.  That is also the whole remaining job of SELF(): a header
        # that has copies cannot name those three itself.
        #
        copies_match = re.search(r'//[ \t]*COPIES:[ \t]*(\d+)', content)
        copies = int(copies_match.group(1)) if copies_match else 1
        if copies < 1:
            raise SystemExit(f"{header_path}: COPIES: {copies} - an effect "
                             f"that exists no times is a file you can delete")

        def_mix_match = re.search(r'//\s*DEFAULT_MIX:\s*(\S+)', content)
        def_mix = float(def_mix_match.group(1)) if def_mix_match else 1.0

        #
        # 'MIX:' answers two independent questions, in either order and
        # both with a default: how the wet and the dry go together (see
        # 'enum mix_law'), and how much of the signal the effect wants to
        # see - MONO for the left channel only, STEREO for both, or
        # NONE for something that is not in the mix at all.
        #
        # Only the uppercase run is taken, so the trailing '// why' that
        # these lines tend to carry stays out of it.
        #
        mix_law, channels = 'LINEAR', 'MONO'
        mix_match = re.search(r'//[ \t]*MIX:[ \t]*([A-Z \t]*)', content)
        for word in mix_match.group(1).split() if mix_match else []:
            if word in ('LINEAR', 'POWER'):
                mix_law = word
            elif word in ('MONO', 'STEREO', 'NONE'):
                channels = word
            else:
                sys.exit(f"gen_effects: {filename}: unknown MIX: option "
                         f"'{word}' (want LINEAR/POWER and "
                         f"MONO/STEREO/NONE)")

        #
        # The pots, and what each one is for.
        #
        # Read a line at a time rather than swept out of the whole file,
        # because 'INFO:' attaches to the POT: line above it and
        # adjacency is not something a search of the text can express.
        # It also means a complaint can name the line it is about.
        #
        # Match: // POT: "Name" CURVE(a b c) = 1.0 Unit
        pot_re = re.compile(r'//[ \t]*POT:[ \t]*"([^"]+)"[ \t]+(LINEAR|FREQUENCY|SQUARED|EXPONENTIAL|RAW|ENUM)(?:\(([^)]+)\))?(?:[ \t]*=[ \t]*(\S+))?(?:[ \t]+(\S+))?[ \t]*$')
        info_re = re.compile(r'//[ \t]*INFO:[ \t]*(.*?)[ \t]*$')

        pots = []
        open_pot = None     # the pot an INFO: line would belong to

        for lineno, line in enumerate(content.splitlines(), 1):
            line = line.rstrip()

            #
            # A sentence or three about what this control does, for the
            # app to show when somebody hovers over it.  Several lines
            # run together into one paragraph, so it can be wrapped here
            # the way the rest of the file is.
            #
            # Only directly under its POT:, or under another INFO:.
            # Anywhere else it is prose that happens to start with the
            # word, and quietly attaching it to whichever pot came last
            # would be worse than refusing.
            #
            info = info_re.match(line)
            if info:
                if not open_pot:
                    raise SystemExit(f"{header_path}:{lineno}: INFO: does not "
                                     f"follow a POT: line")
                open_pot['info'].append(info.group(1))
                continue

            m = pot_re.match(line)
            if not m:
                open_pot = None
                continue

            p_label, p_curve, p_args, p_def, p_unit = m.groups()

            enum_list = None
            if p_curve == 'ENUM' and p_args:
                enum_list = p_args.split()

            default_val = 0.0
            if p_def:
                if enum_list and p_def in enum_list:
                    default_val = float(enum_list.index(p_def))
                else:
                    try:
                        default_val = float(p_def)
                    except ValueError:
                        default_val = 0.0

            open_pot = {
                'label': p_label,
                'ident': c_ident(header_path, 'pot label', p_label),
                'unit': p_unit if p_unit else "none",
                'curve': p_curve,
                'args': p_args.split() if p_args else [],
                'enum': enum_list,
                'default': default_val,
                'info': [],
            }
            pots.append(open_pot)

        for pot in pots:
            pot['info'] = ' '.join(pot['info']) or None

        #
        # Two labels that come out as one identifier would define one
        # constant twice, and the compiler would blame the second
        # definition rather than the pair that caused it.  Name both.
        #
        seen_idents = {}
        for pot in pots:
            first = seen_idents.setdefault(pot['ident'].upper(), pot['label'])
            if first != pot['label']:
                raise SystemExit(f"{header_path}: pot labels '{first}' and "
                                 f"'{pot['label']}' both come out as "
                                 f"'{pot['ident'].upper()}'")

        #
        # An effect whose controls are better drawn than listed says so
        # here, and says what the picture is made of.
        #
        # 'GRAPH: LOSHELF PEAKING HISHELF' is three filter sections, and
        # the app draws the response of that cascade with a draggable
        # node per section.  The pots are taken in order, two per band,
        # frequency then gain - which is a convention rather than
        # something declared, because a band that did not have both
        # would have nothing to be dragged around in.
        #
        # It exists because the app used to recognise the one effect
        # that wanted this by name, which meant renaming that effect
        # silently turned its curve into ten sliders.  Now the effect
        # declares what it is and the app draws whatever is declared -
        # still a special case, but a general one, and open to any
        # effect that wants a picture.
        #
        # A band may carry its Q as 'PEAKING:0.707'.  Default 1.0, which
        # is what the only graphed effect used before there was anywhere
        # to say otherwise.
        #
        # Or it may name a pot - 'PEAKING:MID_Q' - and then the Q can be
        # moved while playing.  A pot is named the way the generated
        # constant names it, which is the label with the spaces turned
        # into underscores; it used to be 'POT6', a position that had to
        # be recounted every time the POT: lines above it moved.
        #
        by_ident = {pot['ident'].upper(): n for n, pot in enumerate(pots)}
        graph_match = re.search(r'//[ \t]*GRAPH:[ \t]*([A-Z0-9._: \t]*)', content)
        graph = []

        for word in graph_match.group(1).split() if graph_match else []:
            kind, _, q = word.partition(':')
            if kind not in ('LOSHELF', 'PEAKING', 'HISHELF'):
                raise SystemExit(f"{header_path}: GRAPH: unknown band '{kind}' "
                                 f"(want LOSHELF/PEAKING/HISHELF)")
            if q in by_ident:
                graph.append({'type': kind, 'q_pot': by_ident[q]})
                continue
            try:
                q = float(q) if q else 1.0
            except ValueError:
                raise SystemExit(f"{header_path}: GRAPH: '{word}' is neither a "
                                 f"number nor one of this effect's pots "
                                 f"({', '.join(by_ident) or 'it has none'})")
            if q <= 0.0:
                raise SystemExit(f"{header_path}: GRAPH: Q must be positive, got {q}")
            graph.append({'type': kind, 'q': q})
        if graph and len(graph) * 2 > len(pots):
            raise SystemExit(f"{header_path}: GRAPH: declares {len(graph)} bands "
                             f"but there are only {len(pots)} pots to make "
                             f"{len(graph) * 2} of them from")

        #
        # What a pot is *for*, where the app needs to know.
        #
        # 'ROLE: CHANNEL:MIDI_CH' says that the MIDI Ch pot is the MIDI
        # channel, in the same shape GRAPH: names a pot for a band's Q.
        # The app used to find it by matching the label "MIDI Ch", so
        # renaming that label quietly stopped the app tracking the
        # channel - quietly, because the fallback is the old behaviour of
        # transmitting on channel 1, which works fine until somebody sets
        # a channel.  Naming it here still tracks the label, but a label
        # that no longer exists is now a build failure rather than a
        # silence.
        #
        # Nothing here knows what a role means; that is the app's
        # business.  This only carries the fact that a pot has one.
        #
        roles = {}
        role_match = re.search(r'//[ \t]*ROLE:[ \t]*([A-Z0-9_: \t]*)', content)

        for word in role_match.group(1).split() if role_match else []:
            role, _, which = word.partition(':')
            if which not in by_ident:
                raise SystemExit(f"{header_path}: ROLE: '{word}' does not name "
                                 f"one of this effect's pots "
                                 f"({', '.join(by_ident) or 'it has none'})")
            if role in roles:
                raise SystemExit(f"{header_path}: ROLE: '{role}' named twice")
            roles[role] = by_ident[which]

        sources.append({
            'id': effect_id,
            'base': base,
            #
            # Every C name this effect owns is built from the short name:
            # lowercased for functions and variables, uppercased for
            # constants.  The short name used to be the label in the
            # effect selector on a display the pedal no longer has, which
            # is why a few of them still read as five-character
            # abbreviations.  What it is now is the effect's name in the
            # generated code, which leaves the filename free to be
            # descriptive - parametric_eq.h is EQ, signal_chain.h is
            # CHAIN.
            #
            'prefix': c_ident(header_path, 'short name', short_name).lower(),
            'copies': copies,
            'graph': graph,
            'roles': roles,
            'full_name': full_name,
            'short_name': short_name,
            'priority': priority,
            'def_mix': def_mix,
            'mix_law': mix_law,
            'channels': channels,
            'pots': pots,
            'header_path': header_path
        })

    # Sort by priority, then by filename to break ties.
    #
    # Priorities are a hint about where an effect wants to sit in the
    # chain, not a total order, so several effects sharing one is fine
    # and expected.  What is not fine is the tie-break coming from
    # os.listdir(), which is inode order: that made the ids depend on
    # the filesystem, and the ids are what end up in saved scenes and on
    # the wire.  Filenames are unique in a directory, so this key is a
    # total order and the result is the same everywhere.
    sources.sort(key=lambda x: (x['priority'], x['base']))

    #
    # Every generated name hangs off the prefix, and they all land in one
    # translation unit, so two effects sharing one is a pile of
    # redefinitions rather than a style problem.  Filenames cannot
    # collide - a directory sees to that - but short names are written by
    # hand and nothing else checks them.
    #
    by_prefix = {}
    for src in sources:
        first = by_prefix.setdefault(src['prefix'], src)
        if first is not src:
            raise SystemExit(f"{src['header_path']}: short name "
                             f"'{src['short_name']}' gives the prefix "
                             f"'{src['prefix']}', which "
                             f"{first['header_path']} already uses")

    #
    # One file becomes one effect, or several.  The copies land next to
    # each other because they share every part of the sort key, so the
    # ids come out in the order the file asked for them and the first
    # copy keeps the id the single effect had.
    #
    # The first copy is named for the effect and the rest are numbered
    # from two - 'tone' and 'tone2', which is what the symlink produced
    # back when the second copy was a second filename.  Keeping that
    # spelling is not nostalgia: those names are in the ELF, in every map
    # file, and in whatever anybody has been reading while debugging.
    #
    for src in sources:
        for copy in range(src['copies']):
            e = dict(src)
            e['copy'] = copy
            e['self_name'] = src['prefix'] if not copy else f"{src['prefix']}{copy + 1}"
            effects_data.append(e)

    #
    # A file included twice brings its display name twice with it, and
    # two chips both reading "Tone" are two chips nobody can tell apart.
    # They are genuinely interchangeable, so numbering them is honest as
    # well as sufficient: it says "these are the same thing, and this is
    # the second one".
    #
    # After the sort, so which one is 1 comes from the same deterministic
    # order the ids do rather than from the order the directory was read
    # in.
    #
    name_counts = Counter(e['full_name'] for e in effects_data)
    seen = Counter()
    for e in effects_data:
        if name_counts[e['full_name']] > 1:
            seen[e['full_name']] += 1
            e['full_name'] = f"{e['full_name']} {seen[e['full_name']]}"

    # Auto-assign index as ID
    for i, e in enumerate(effects_data):
        e['id'] = i

    # Build maps
    for e_idx, e_data in enumerate(effects_data):
        ui_pots = []
        for pot in e_data['pots']:
            min_v = 0.0
            max_v = 1.0
            if pot['curve'] != 'ENUM' and len(pot['args']) >= 2:
                min_v = float(pot['args'][0])
                max_v = float(pot['args'][1])
            elif pot['curve'] == 'ENUM' and pot['enum']:
                max_v = float(len(pot['enum']) - 1)

            pot['pot_val'] = default_pot_value(pot)

            ui_pots.append({
                "name": pot['label'],
                "unit": pot['unit'],
                "curve": pot['curve'],
                "min": min_v,
                "max": max_v,
                "default": pot['default'],
                "defaultPot": pot['pot_val'],
                "enum": pot['enum'],
                "info": pot['info']
            })

        ui_effects.append({
            "id": e_data['id'],
            #
            # Whether it has anywhere to sit across the two channels.
            # An effect with no step() is never handed to
            # do_effect_step(), so steering it would mean nothing -
            # which is the same condition that gives it no mix.
            #
            "steerable": e_data['channels'] != 'NONE',
            "base": e_data['base'],
            "name": e_data['full_name'],
            "shortName": e_data['short_name'],
            "defMix": e_data['def_mix'],
            "mixLaw": e_data['mix_law'],
            # The schema is camelCase, the python is not
            "roles": e_data['roles'],
            "graph": [{"type": b['type'], "q": b['q']} if 'q' in b
                      else {"type": b['type'], "qPot": b['q_pot']}
                      for b in e_data['graph']],
            "pots": ui_pots
        })

    # Generate effect_map.h
    with open(out_h, 'w') as f:
        f.write("// Auto-generated by gen_effects.py\n")
        #
        # How an effect header names things without knowing its own
        # name.  Two levels because the argument has to be expanded
        # before it is pasted, which is the usual preprocessor tax.
        #
        f.write("#define _EFFECT_PASTE(a, b) a##b\n")
        f.write("#define _EFFECT_EXPAND(a, b) _EFFECT_PASTE(a, b)\n")
        f.write("#define SELF(suffix) _EFFECT_EXPAND(EFFECT_SELF, suffix)\n\n")

        #
        # Everything an effect's copies have in common, emitted once
        # whether there is one copy or three.  A pot accessor and the Q
        # table are pure functions of the pot array - two copies would be
        # the same code twice, and the second one would be the one that
        # got edited.
        #
        # This is also why it is a pass of its own rather than the head
        # of the per-copy loop: a shared thing has to exist before every
        # copy that uses it, and "before" is easier to guarantee by
        # writing it all out first than by reasoning about the sort.
        #
        for src in sources:
            prefix = src['prefix']

            #
            # Which pot is which.  This used to be written by hand in the
            # two effects that wanted it, with a comment saying it had to
            # stay in step with the POT: lines above it - and naming one
            # of the two numbers that had to agree is exactly what let
            # them disagree, because signal_chain_pot3(pot[CHAIN_TRIM])
            # is not something you can check by reading it.
            #
            if src['pots']:
                f.write(f"enum {prefix}_pot {{\n")
                for pot in src['pots']:
                    pot['const'] = f"{prefix}_{pot['ident']}".upper()
                    f.write(f"\t{pot['const']},\n")
                f.write("};\n")

            for pot in src['pots']:
                fn_name = f"{prefix}_{pot['ident'].lower()}_pot"
                pot['fn_name'] = fn_name

                if pot['curve'] == 'ENUM' and pot['enum']:
                    enum_name = f"{prefix}_{pot['ident'].lower()}_enum"
                    pot['enum_name'] = enum_name
                    f.write(f"static const char *const {enum_name}[] = {{ ")
                    for val in pot['enum']:
                        f.write(f'"{val}", ')
                    f.write("NULL };\n")

                #
                # The accessor does its own indexing, so the pot is named
                # once at the call site and there is no second number to
                # get wrong.
                #
                val = f"pot[{pot['const']}]"
                args_str = ", ".join(pot['args'])
                sig = f"static float {fn_name}(const unsigned char pot[10])"
                if pot['curve'] == 'RAW' or pot['curve'] == 'ENUM':
                    f.write(f"{sig} {{ return {val}; }}\n")
                elif pot['curve'] == 'LINEAR':
                    f.write(f"{sig} {{ return linear_pot({val}, {args_str}); }}\n")
                elif pot['curve'] == 'FREQUENCY':
                    f.write(f"{sig} {{ return frequency_pot({val}, {args_str}); }}\n")
                elif pot['curve'] == 'SQUARED':
                    f.write(f"{sig} {{ float p = POT_TO_FLOAT({val}); return linear(p*p, {args_str}); }}\n")
                elif pot['curve'] == 'EXPONENTIAL':
                    a_val = float(pot['args'][0])
                    b_val = float(pot['args'][1])
                    log2_ratio = math.log2(b_val / a_val) if a_val != 0 else 0
                    f.write(f"{sig} {{ float p = POT_TO_FLOAT({val}); return {a_val}f * pow2(p * {log2_ratio}f); }}\n")

            #
            # The Q of each graphed band, from the same declaration the
            # app draws from.  It used to be a constant in the effect
            # header and a different constant in the app, which is two
            # places for one number and they had already diverged: the
            # app drew every band at 1.0 while the tone control ran at
            # 0.707, so the picture was not the filter.
            #
            # Written into an array at init rather than read where it is
            # used, so there is exactly one call site whatever the bands
            # turn out to be - a helper with several would risk becoming
            # a real call, and this runs on the audio core.
            #
            if src['graph']:
                f.write(f"static inline void {prefix}_graph_q("
                        f"float *q, const unsigned char pot[10])\n{{\n")
                for n, b in enumerate(src['graph']):
                    if 'q_pot' in b:
                        f.write(f"\tq[{n}] = "
                                f"{src['pots'][b['q_pot']]['fn_name']}(pot);\n")
                    else:
                        f.write(f"\tq[{n}] = {b['q']}f;\n")
                f.write("}\n")
            f.write("\n")

        #
        # ...and then what each copy owns for itself: its state, its two
        # entry points, and the struct that names them.
        #
        for e_data in effects_data:
            base = e_data['base']
            self_name = e_data['self_name']
            struct_name = f"{self_name}_effect"
            e_data['struct_name'] = struct_name

            f.write(f"static struct effect {struct_name};\n")

            # Declare the two entry points ahead of the header, marked as
            # running on the audio core. Gcc carries a section attribute
            # from the declaration to the definition, so the effect headers
            # don't have to know about any of this.
            channels = e_data['channels']

            f.write(f"static void __audio_func({self_name}_init)(unsigned char[10]);\n")
            if channels in ('STEREO', 'NONE'):
                f.write(f"static sample_t __audio_func({self_name}_step)(sample_t);\n")
            else:
                f.write(f"static float __audio_func({self_name}_step)(float);\n")
            #
            # A header with copies refers to itself as SELF(_thing)
            # rather than writing a name it cannot know.  One file is
            # then two effects that cannot drift apart, because there is
            # only one of them - and SELF() covers just the three things
            # a copy owns, because everything else was emitted above and
            # is shared.
            #
            f.write(f"#define EFFECT_SELF {self_name}\n")
            f.write(f"#include \"../effects/{base}.h\"\n")
            f.write("#undef EFFECT_SELF\n")

            # The wrapper the chain calls - see do_effect_step().
            #
            # It runs the effect and hands back what it produced, and
            # that is all.  Where the answer goes is the caller's, so
            # that per-effect channel routing lives in one place instead
            # of being generated into eighteen effects.
            #
            # The mono and stereo difference is still here, because it
            # is a property of the effect rather than of the caller: a
            # mono one has a single answer and puts it in both halves, a
            # stereo one answers for each channel.  do_effect_step()
            # places whichever it is handed without needing to know
            # which it got.
            #
            # This is the only call site of {self_name}_step(), so it
            # inlines here and the chain is still one indirect call per
            # effect.
            #
            # NONE gets no wrapper and no .step at all - nothing about
            # the chain's wet and dry describes it, and whoever does call
            # it calls it by name.
            step = None
            if channels != 'NONE':
                step = f"{self_name}_raw_step"
                f.write(f"static sample_t __audio_func({step})(sample_t val)\n")
                f.write("{\n")
                if channels == 'STEREO':
                    f.write(f"\treturn {self_name}_step(val);\n")
                else:
                    f.write(f"\tfloat out = {self_name}_step(val.left);\n")
                    f.write("\tval.left = val.right = out;\n")
                    f.write("\treturn val;\n")
                f.write("}\n")

            f.write(f"static struct effect {struct_name} = {{\n")
            f.write(f"\t.name = \"{e_data['full_name']}\",\n")
            f.write(f"\t.short_name = \"{e_data['short_name']}\",\n")
            f.write(f"\t.id_hash = 0x{effect_id_hash(e_data['short_name'], e_data['copy']):08x},\n")
            f.write(f"\t.pot_hash = 0x{pot_layout_hash(e_data['pots']):08x},\n")
            f.write(f"\t.def_mix = {e_data['def_mix']}f,\n")
            f.write(f"\t.mix_law = MIX_{e_data['mix_law']},\n")
            if e_data['channels'] == 'STEREO':
                f.write("\t.stereo = 1,\n")
            f.write(f"\t.init = {self_name}_init,\n")
            if step:
                f.write(f"\t.step = {step},\n")
            else:
                f.write("\t.no_mix = 1,\n")
            f.write(f"\t.pots = {{\n")

            for p_idx, pot in enumerate(e_data['pots']):
                pot_val = pot['pot_val']

                unit_str = f"\"{pot['unit']}\"" if pot['unit'] and pot['unit'] != "none" else "NULL"
                enum_str = f", {pot['enum_name']}" if 'enum_name' in pot else ""
                f.write(f"\t\tEFFECT_POT(\"{pot['label']}\", {unit_str}, {pot['fn_name']}, {pot_val}{enum_str}),\n")

            f.write("\t}\n")
            f.write("};\n\n")

        #
        # In RAM, not in flash.  single_sample() indexes this every
        # sample on the audio core, and the audio core has to keep
        # running while core 0 erases a flash sector with XIP switched
        # off - see check-audio.py, which will not let it back into
        # .rodata by accident.
        #
        f.write("static struct effect *const __not_in_flash(\"audio\")"
                " effects[] = {\n")
        for e_data in effects_data:
            f.write(f"\t&{e_data['struct_name']},\n")
        f.write("};\n\n")

        f.write(f"#define EFFECT_COUNT {len(effects_data)}\n\n")

    # Generate midi_schema.h next to effect_map.h
    schema_path = os.path.join(os.path.dirname(out_h), "midi_schema.h")
    with open(schema_path, 'w') as f:
        f.write("// Auto-generated by gen_effects.py\n")
        #
        # The three parameters every steerable effect has, said once.
        #
        # They are not declared by any effect header, because they are
        # not about what an effect does - they are the same question
        # asked of all of them, like the mix.  Sent as one block rather
        # than repeated into every effect's pot list because the schema
        # and the state dump are already big enough to disturb the audio
        # (issue 78), and sixteen copies of this would be for nothing.
        #
        # The indices are the wire numbering: 0 is the mix, 1..10 are the
        # effect's own, and these follow.
        #
        steering = {"pots": [
            {"name": "In", "index": 11, "curve": "ENUM",
             "enum": ["Left", "Right"], "default": 0,
             "info": "Which channel this effect listens to."},
            {"name": "Out", "index": 12, "curve": "ENUM",
             "enum": ["Both", "Left", "Right", "Merge"], "default": 0,
             "info": "Where its answer goes. The channel it does not "
                     "write keeps whatever it had, which is what lets a "
                     "split survive several effects."},
            {"name": "Merge", "index": 13, "curve": "LINEAR",
             "min": 0.0, "max": 1.0, "default": 1.0, "defaultPot": 120,
             "info": "How much of the untouched channel a Merge keeps. "
                     "At 1.0 a split and a merge with nothing in between "
                     "comes back at unity."},
        ]}
        json_str = json.dumps({"steering": steering, "effects": ui_effects},
                              separators=(',', ':'))
        f.write(f'static const char *const midi_schema_json = "{c_string(json_str)}";\n')

    #
    # The CC numbers and status bits the app needs are read out of
    # midi.h rather than written down a second time here.  Two copies of
    # a protocol agree right up until they don't, and this file is how
    # the web app learns what the firmware speaks.
    #
    midi_h = os.path.join(os.path.dirname(os.path.abspath(audio_dir)), "midi.h")
    with open(midi_h) as f:
        midi_src = f.read()

    def midi_const(name):
        m = re.search(r'#define\s+%s\s+(.*)' % name, midi_src)
        if not m:
            sys.exit(f"gen_effects: {midi_h}: no #define for {name}")
        val = m.group(1).split('//')[0].strip()
        shift = re.fullmatch(r'\(1u << (\d+)\)', val)
        return (1 << int(shift.group(1))) if shift else int(val, 0)

    js_consts = [
        ("GLOBAL_ENABLE_CC", "MIDI_CC_GLOBAL_ENABLE"),
        ("STATUS_GLOBAL_CC", "MIDI_CC_STATUS_GLOBAL"),
        ("STATUS_CHAIN_LO_CC", "MIDI_CC_STATUS_CHAIN_LO"),
        ("STATUS_CHAIN_HI_CC", "MIDI_CC_STATUS_CHAIN_HI"),
        ("STATUS_DROPPED_MASK", "STATUS_DROPPED_MASK"),
        ("STATUS_CLIPPED", "STATUS_CLIPPED"),
        ("STATUS_FRONT_ATTN", "STATUS_FRONT_ATTN"),
        ("STATUS_CHAIN_BITS", "STATUS_CHAIN_BITS"),
    ]

    with open(out_js, 'w') as f:
        f.write("// Auto-generated by gen_effects.py\n")
        # We don't output PEDAL_EFFECTS here anymore, the webapp will fetch it dynamically!
        f.write(f"// CC numbers and status bits taken from {os.path.basename(midi_h)}\n")
        for js_name, c_name in js_consts:
            f.write(f"const {js_name} = {midi_const(c_name)};\n")

    with open(out_md, 'w') as f:
        f.write("# MIDI Implementation\n\n")
        #
        # Hand-written, because these are not derived from the effects -
        # but kept to what the firmware actually implements.  This file
        # is generated and published, so anything listed here is a claim
        # about a protocol somebody may go and speak.
        #
        f.write("Anything invented here lives in CC 102-119, which the MIDI\n")
        f.write("spec leaves undefined. CC 20 is the exception and is frozen\n")
        f.write("where it is: value 126 on it is how an enclosed pedal gets\n")
        f.write("into programming mode, so it has to keep answering the\n")
        f.write("number that already-flashed firmware knows.\n\n")

        f.write("## Control Change, in\n\n")
        f.write("- **CC 7:** Main volume - the Volume pot of the Signal\n")
        f.write("  Chain, applied at the end of the chain. 0 is silence;\n")
        f.write("  above that it is -40dB to +20dB, linear in dB, reaching\n")
        f.write("  unity two thirds of the way up.\n")
        f.write("- **CC 20:** Global bypass, with three values that mean\n")
        f.write("  something else instead: 68 enters tuner mode, 69 leaves\n")
        f.write("  it, and 126 reboots to the bootloader. 0 bypasses and\n")
        f.write("  anything else enables.\n\n")

        f.write("## Control Change, out\n\n")
        f.write("- **CC 20:** the pedal's own bypass state as 0 or 127, and\n")
        f.write("  68/69 when a long press moves it in or out of tuner mode.\n")
        f.write("- **CC 102:** global status. Sent when it changes, and\n")
        f.write("  repeated about three times a second regardless, so that\n")
        f.write("  a dropped message or a host that connected late cannot\n")
        f.write("  leave anyone believing a stale answer.\n")
        f.write("  - bits 0-4: samples dropped since the last report,\n")
        f.write("    saturating at 31. A count and not a flag, because\n")
        f.write("    dropping one sample and dropping them steadily are\n")
        f.write("    different problems.\n")
        f.write("  - bit 5 (32): the output clipped.\n")
        f.write("  - bit 6 (64): the effect at the front of the chain wants\n")
        f.write("    attention - the noise gate is closed.\n")
        f.write("- **CC 103, CC 104:** one bit per routed effect in chain\n")
        f.write("  order, set while that effect wants attention: the\n")
        f.write("  compressor is compressing, the boost is clipping, the\n")
        f.write("  echo is in sound-on-sound. CC 103 is chain positions\n")
        f.write("  0-6, CC 104 is positions 7-13.\n\n")

        f.write("## SysEx queries\n\n")
        f.write("Two, and they are encoded differently on purpose.\n\n")
        f.write("- **0x0a, identity.** Asked once, when a host connects, so\n")
        f.write("  it replies with JSON like the schema does: self-describing,\n")
        f.write("  and a new field costs nobody a version negotiation. Carries\n")
        f.write("  the build timestamp, how many scenes this pedal has, and\n")
        f.write("  what answered on the i2c bus.\n")
        f.write("- **0x0b, telemetry.** Polled while somebody is watching a\n")
        f.write("  meter, so it replies with packed bytes:\n")
        f.write("  `F0 7D 0B <version> <in> <floor> <out> <gate> <load> F7`.\n")
        f.write("  Levels are -dBFS, one byte per dB, counting down from\n")
        f.write("  full scale. **0dBFS is 1Vrms at the input** - the internal\n")
        f.write("  scale is arranged so that a 1Vrms sine peaks at 1.0, see\n")
        f.write("  `audio/process.h`.\n")
        f.write("- `<in>` is the input peak before Trim, and `<floor>` the\n")
        f.write("  quiet level under it. The floor follows the *gate's* own\n")
        f.write("  envelope rather than the peak meter, so that it is the\n")
        f.write("  same quantity the gate's own setting is compared\n")
        f.write("  against - measured any other way it would only happen to\n")
        f.write("  agree. In practice the floor lands within a couple of dB\n")
        f.write("  of where the gate starts closing.\n")
        f.write("- `<out>` is the output peak after Volume. `<gate>` is 127\n")
        f.write("  open and 0 fully closed. `<load>` is the share of each\n")
        f.write("  sample period spent working, out of 127, measured by\n")
        f.write("  timing the spin that waits for the next sample. An idle\n")
        f.write("  pedal reads about 4 rather than 0: the timer is 1us\n")
        f.write("  against a 20.83us period, so that is quantisation.\n\n")
        f.write("  **The telemetry layout is append-only.** Fields are never\n")
        f.write("  reordered, resized or repurposed. Read the ones you know\n")
        f.write("  and ignore the rest; a shorter frame than you expected\n")
        f.write("  means that firmware predates the tail, not that the tail\n")
        f.write("  is zero.\n\n")

        f.write("## SysEx control bindings\n\n")
        f.write("What the pedal's own knob and footswitch do. There is no\n")
        f.write("screen, so nothing on the pedal can say what a gesture is\n")
        f.write("for - the answer is set from here instead of being compiled\n")
        f.write("in.\n\n")
        f.write("It is a flat table of rules, up to 16 of them, six bytes\n")
        f.write("each: `<control> <action> <effect> <pot> <v0> <v1>`. A\n")
        f.write("control may appear more than once, and every rule naming a\n")
        f.write("gesture fires when that gesture happens - so one press can\n")
        f.write("take one effect's mix up while taking another's down, which\n")
        f.write("is how you swap between two effects inside one scene.\n\n")
        f.write("- **0x0c, the whole table, in.** `F0 7D 0C <rules...> F7`.\n")
        f.write("  Rules that do not check out are dropped and the rest kept,\n")
        f.write("  so the reply answers *what did you keep* rather than\n")
        f.write("  acknowledging. One bad rule does not lose the other\n")
        f.write("  fifteen.\n")
        f.write("- **0x0d, the whole table, out.** The same bytes back. Sent\n")
        f.write("  as part of the state dump, in reply to 0x0c, and again\n")
        f.write("  whenever the pedal moves a rule by itself.\n\n")
        f.write("Controls: 0 rotary turn,\n")
        f.write("1 rotary tap, 2 rotary long press, 3 stomp tap, 4 stomp long\n")
        f.write("press. Turning the rotary with its shaft held down is\n")
        f.write("deliberately not one of them - it cannot coexist with a long\n")
        f.write("press on the same shaft, because holding the shaft in order\n")
        f.write("to turn it manufactures one every time.\n\n")
        f.write("Actions: 0 none, 1 drive the target, 2 step the knob to\n")
        f.write("the next pot, 3 reset the target to its default, 4 set the\n")
        f.write("target to `v0`, 5 toggle the target between `v0` and `v1`,\n")
        f.write("6 bypass, 7 tuner, 8 load scene (`effect` is the scene).\n\n")
        f.write("Actions 1 and 3-5 carry a target, `<effect> <pot>`, and it\n")
        f.write("need not be the parameter the knob turns. `<pot>` numbers\n")
        f.write("the parameter the same way the parameter write above does:\n")
        f.write("**0 is the mix**, 1-10 are the effect's own pots. Toggling\n")
        f.write("a mix between 0 and 100% is how a footswitch turns one\n")
        f.write("effect on and off. A target `effect` of\n")
        f.write("**0x7f** means *the pot the knob is currently turning*,\n")
        f.write("resolved when the gesture happens rather than when the\n")
        f.write("binding is set, so that a press which resets the knob\n")
        f.write("follows the knob when it is rebound. That is accepted only\n")
        f.write("for action 3: a value is meaningless until the pot is\n")
        f.write("known, since 80 is unity on the master volume and an\n")
        f.write("arbitrary number of milliseconds on a delay time.\n\n")
        f.write("Values are clamped to the target pot's range when the\n")
        f.write("gesture happens, not when the binding is set - a following\n")
        f.write("target has no range yet to be checked against.\n")
        f.write("Action 1 is the only one that means anything for a control\n")
        f.write("that turns, and the rest are the only ones that mean\n")
        f.write("anything for one that clicks; the pedal does not enforce\n")
        f.write("that, because a useless binding is not a dangerous one.\n\n")
        f.write("**There are four tables, and 0x0c/0x0d carry which one**\n")
        f.write("as a byte after the command: 0 the current scene's rules, 1\n")
        f.write("the pedal-wide ones, 2 what those resolve to, 3 what is\n")
        f.write("compiled in. Levels 0 and 1 are settings and are saved with\n")
        f.write("the scene and the globals; 2 and 3 are answers, and a write\n")
        f.write("to either is refused rather than quietly dropped.\n\n")
        f.write("Resolution is per control, and the most specific level that\n")
        f.write("mentions a control at all answers for it completely. Not a\n")
        f.write("merge: every rule naming a gesture fires, so a union would\n")
        f.write("let a scene add to the footswitch's binding but never\n")
        f.write("replace it. Which is what makes action 0 useful - a rule\n")
        f.write("that does nothing still counts as the scene having spoken.\n\n")
        f.write("Delete every rule at both levels and the compiled-in ones\n")
        f.write("come back, so a pedal cannot be configured into having no\n")
        f.write("way out. They are: the rotary is the master volume, both of\n")
        f.write("its presses reset that volume to unity, the footswitch\n")
        f.write("tapped is bypass and held is the tuner.\n\n")
        f.write("Both rotary presses do the same thing on purpose. A press\n")
        f.write("held a moment too long arrives as a long press and not as a\n")
        f.write("short one, so binding them differently would make a slow\n")
        f.write("press do the wrong thing silently - and the way back to a\n")
        f.write("known value is the last control that should care about\n")
        f.write("timing on a pedal you cannot see.\n\n")

        f.write("## Program Change (Scenes)\n\n")
        f.write("- **PC 0-31:** Load Scene 0-31 from EEPROM, in. Nothing\n")
        f.write("  sends Program Change out.\n\n")

        f.write("## SysEx Deep Editing\n\n")
        f.write("The pedal uses SysEx messages for deep editing and dynamic feature discovery. Header: `F0 7D`.\n\n")
        f.write("### Effects Reference\n\n")
        for e_idx, e_data in enumerate(effects_data):
            f.write(f"#### {e_data['full_name']} (ID: {e_data['id']})\n\n")
            for p_idx, pot in enumerate(e_data['pots']):
                if pot['curve'] == 'ENUM' and pot['enum']:
                    range_str = ", ".join([f"{i}={v}" for i, v in enumerate(pot['enum'])])
                    input_str = f"Index: {p_idx} (0-{len(pot['enum'])-1}, maps to: {range_str})"
                elif pot['curve'] == 'RAW':
                    input_str = f"Index: {p_idx} (0-127, Raw value)"
                else:
                    if len(pot['args']) >= 2:
                        min_val = pot['args'][0]
                        max_val = pot['args'][1]
                        unit = pot['unit']
                        if unit and unit != "none":
                            range_str = f"{min_val} to {max_val} {unit}"
                        else:
                            range_str = f"{min_val} to {max_val}"
                    else:
                        range_str = "0.0 to 1.0"
                    input_str = f"Index: {p_idx} (0-120, maps to: {range_str})"
                f.write(f"- **{pot['label']}:** {input_str}\n")
                if pot['info']:
                    f.write(f"  - {pot['info']}\n")
            f.write("\n")

if __name__ == "__main__":
    if len(sys.argv) < 5:
        print("Usage: gen_effects.py <audio_dir> <out_h> <out_js> <out_md>")
        sys.exit(1)

    generate(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
