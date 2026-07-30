#!/usr/bin/env python3
import sys
import os
import re
import json
import math

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
    effects_data = []

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

        pots = []
        # Match: // POT: "Name" CURVE(a b c) = 1.0 Unit
        pot_lines = re.findall(r'//[ \t]*POT:[ \t]*"([^"]+)"[ \t]+(LINEAR|FREQUENCY|SQUARED|EXPONENTIAL|RAW|ENUM)(?:\(([^)]+)\))?(?:[ \t]*=[ \t]*(\S+))?(?:[ \t]+(\S+))?[ \t]*\n?', content)

        for p_label, p_curve, p_args, p_def, p_unit in pot_lines:
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

            pots.append({
                'label': p_label,
                'unit': p_unit if p_unit else "none",
                'curve': p_curve,
                'args': p_args.split() if p_args else [],
                'enum': enum_list,
                'default': default_val
            })

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
        graph_match = re.search(r'//[ \t]*GRAPH:[ \t]*([A-Z \t]*)', content)
        graph = graph_match.group(1).split() if graph_match else []

        for band in graph:
            if band not in ('LOSHELF', 'PEAKING', 'HISHELF'):
                raise SystemExit(f"{header_path}: GRAPH: unknown band '{band}' "
                                 f"(want LOSHELF/PEAKING/HISHELF)")
        if graph and len(graph) * 2 > len(pots):
            raise SystemExit(f"{header_path}: GRAPH: declares {len(graph)} bands "
                             f"but there are only {len(pots)} pots to make "
                             f"{len(graph) * 2} of them from")

        effects_data.append({
            'id': effect_id,
            'base': base,
            'graph': graph,
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
    effects_data.sort(key=lambda x: (x['priority'], x['base']))

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
                "enum": pot['enum']
            })

        ui_effects.append({
            "id": e_data['id'],
            "base": e_data['base'],
            "name": e_data['full_name'],
            "shortName": e_data['short_name'],
            "defMix": e_data['def_mix'],
            "mixLaw": e_data['mix_law'],
            "graph": e_data['graph'],
            "pots": ui_pots
        })

    # Generate effect_map.h
    with open(out_h, 'w') as f:
        f.write("// Auto-generated by gen_effects.py\n")

        for e_data in effects_data:
            base = e_data['base']
            struct_name = f"{base}_effect" if base != "eq" else "EQ"
            e_data['struct_name'] = struct_name

            f.write(f"static struct effect {struct_name};\n")

            for p_idx, pot in enumerate(e_data['pots']):
                fn_name = f"{base}_pot{p_idx}"
                pot['fn_name'] = fn_name

                if pot['curve'] == 'ENUM' and pot['enum']:
                    enum_name = f"{base}_pot{p_idx}_enum"
                    pot['enum_name'] = enum_name
                    f.write(f"static const char *const {enum_name}[] = {{ ")
                    for val in pot['enum']:
                        f.write(f'"{val}", ')
                    f.write("NULL };\n")

                args_str = ", ".join(pot['args'])
                if pot['curve'] == 'RAW' or pot['curve'] == 'ENUM':
                    f.write(f"static float {fn_name}(unsigned char pot) {{ return pot; }}\n")
                elif pot['curve'] == 'LINEAR':
                    f.write(f"static float {fn_name}(unsigned char pot) {{ return linear_pot(pot, {args_str}); }}\n")
                elif pot['curve'] == 'FREQUENCY':
                    f.write(f"static float {fn_name}(unsigned char pot) {{ return frequency_pot(pot, {args_str}); }}\n")
                elif pot['curve'] == 'SQUARED':
                    f.write(f"static float {fn_name}(unsigned char pot) {{ float p = POT_TO_FLOAT(pot); return linear(p*p, {args_str}); }}\n")
                elif pot['curve'] == 'EXPONENTIAL':
                    a_val = float(pot['args'][0])
                    b_val = float(pot['args'][1])
                    log2_ratio = math.log2(b_val / a_val) if a_val != 0 else 0
                    f.write(f"static float {fn_name}(unsigned char pot) {{ float p = POT_TO_FLOAT(pot); return {a_val}f * pow2(p * {log2_ratio}f); }}\n")

            # Declare the two entry points ahead of the header, marked as
            # running on the audio core. Gcc carries a section attribute
            # from the declaration to the definition, so the effect headers
            # don't have to know about any of this.
            channels = e_data['channels']

            f.write(f"static void __audio_func({base}_init)(unsigned char[10]);\n")
            if channels in ('STEREO', 'NONE'):
                f.write(f"static sample_t __audio_func({base}_step)(sample_t);\n")
            else:
                f.write(f"static float __audio_func({base}_step)(float);\n")
            f.write(f"#include \"../effects/{base}.h\"\n")

            # The mixing wrapper, which is what the chain actually calls -
            # see do_effect_step(). A mono effect gets the left channel
            # and its one answer goes to both, which is the behaviour the
            # caller used to impose on every effect alike; a stereo one
            # gets both and answers for both. Written out per effect so
            # that one can stop being mono without every other one having
            # to care.
            #
            # This is the only call site of {base}_step(), so it inlines
            # here and the chain is still one indirect call per effect.
            #
            # NONE gets no wrapper and no .step at all - nothing about
            # the chain's wet and dry describes it, and whoever does call
            # it calls it by name.
            step = None
            if channels != 'NONE':
                step = f"{base}_mix_step"
                f.write(f"static sample_t __audio_func({step})"
                        "(sample_t val, float dry, float wet)\n")
                f.write("{\n")
                if channels == 'STEREO':
                    f.write(f"\tsample_t out = {base}_step(val);\n")
                    f.write("\tval.left  = dry * val.left  + wet * out.left;\n")
                    f.write("\tval.right = dry * val.right + wet * out.right;\n")
                else:
                    f.write(f"\tfloat out = {base}_step(val.left);\n")
                    f.write("\tval.left  = dry * val.left  + wet * out;\n")
                    f.write("\tval.right = dry * val.right + wet * out;\n")
                f.write("\treturn val;\n")
                f.write("}\n")

            f.write(f"static struct effect {struct_name} = {{\n")
            f.write(f"\t.name = \"{e_data['full_name']}\",\n")
            f.write(f"\t.short_name = \"{e_data['short_name']}\",\n")
            f.write(f"\t.def_mix = {e_data['def_mix']}f,\n")
            f.write(f"\t.mix_law = MIX_{e_data['mix_law']},\n")
            f.write(f"\t.init = {base}_init,\n")
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

        f.write("static struct effect *const effects[] = {\n")
        for e_data in effects_data:
            f.write(f"\t&{e_data['struct_name']},\n")
        f.write("};\n\n")

        f.write(f"#define EFFECT_COUNT {len(effects_data)}\n\n")

    # Generate midi_schema.h next to effect_map.h
    schema_path = os.path.join(os.path.dirname(out_h), "midi_schema.h")
    with open(schema_path, 'w') as f:
        f.write("// Auto-generated by gen_effects.py\n")
        json_str = json.dumps(ui_effects, separators=(',', ':'))
        # Escape quotes for C string literal
        json_str = json_str.replace('"', '\\"')
        f.write(f'static const char *const midi_schema_json = "{json_str}";\n')

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
        f.write("  same quantity the gate's Level setting is compared\n")
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
            f.write("\n")

if __name__ == "__main__":
    if len(sys.argv) < 5:
        print("Usage: gen_effects.py <audio_dir> <out_h> <out_js> <out_md>")
        sys.exit(1)

    generate(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
