#!/usr/bin/env python3
"""Exercise gen_effects.py against real effect definitions without hardware."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'scripts' / 'gen_effects.py'


class GeneratedMetadataTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='pedal metadata ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        shutil.copytree(ROOT / 'Effects', self.root / 'Effects')
        midi = self.root / 'Firmware' / 'midi'
        midi.mkdir(parents=True)
        shutil.copyfile(ROOT / 'Firmware' / 'midi' / 'midi.h', midi / 'midi.h')
        self.midi = midi / 'midi.h'
        self.out_h = self.root / 'headers' / 'custom_map.h'
        self.out_js = self.root / 'web app' / 'custom_effects.js'
        self.out_md = self.root / 'docs' / 'custom_map.md'
        self.outputs = [self.out_h, self.out_h.with_name('midi_schema.h'),
                        self.out_js, self.out_md]
        for file in self.outputs:
            file.parent.mkdir(parents=True, exist_ok=True)
        result = self.run_generator()
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_generator(self, check=False, extra=()):
        command = [sys.executable, '-X', 'utf8', str(GENERATOR)]
        if check:
            command.append('--check')
        command += [str(self.root / 'Effects'), str(self.out_h),
                    str(self.out_js), str(self.out_md)]
        return subprocess.run(command + list(extra), capture_output=True,
                              text=True, encoding='utf-8', timeout=15)

    def snapshot(self):
        return {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.outputs
                if p.is_file()}

    def test_current_does_not_write(self):
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_each_stale_output_is_reported_without_writes(self):
        for output in self.outputs:
            with self.subTest(output=output.name):
                original = output.read_bytes()
                output.write_bytes(original + b'\nSTALE\n')
                before = self.snapshot()
                result = self.run_generator(check=True)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(str(output), result.stderr)
                self.assertIn('Regenerate', result.stderr)
                self.assertEqual(self.snapshot(), before)
                output.write_bytes(original)

    def test_each_missing_output_stays_missing(self):
        for output in self.outputs:
            with self.subTest(output=output.name):
                original = output.read_bytes()
                output.unlink()
                before = self.snapshot()
                result = self.run_generator(check=True)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(str(output), result.stderr)
                self.assertFalse(output.exists())
                self.assertEqual(self.snapshot(), before)
                output.write_bytes(original)

    def test_midi_constant_change_stales_javascript(self):
        text = self.midi.read_text()
        changed, count = re.subn(r'(#define\s+MIDI_CC_GLOBAL_ENABLE\s+)20\b',
                                 r'\g<1>21', text)
        self.assertEqual(count, 1)
        self.midi.write_text(changed)
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(str(self.out_js), result.stderr)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.run_generator().returncode, 0)
        self.assertEqual(self.run_generator(check=True).returncode, 0)

    def test_effect_metadata_change_is_detected(self):
        effect = self.root / 'Effects' / 'tone.h'
        text = effect.read_text()
        self.assertIn('// NAME: Tone 1 [TONE]', text)
        effect.write_text(text.replace('// NAME: Tone 1 [TONE]',
                                       '// NAME: Changed Tone [TONE]', 1))
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(str(self.out_h), result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_all_missing_paths_reported_without_creating_directories(self):
        self.out_h = self.root / 'absent headers' / 'map.h'
        self.out_js = self.root / 'absent web' / 'effects.js'
        self.out_md = self.root / 'absent docs' / 'map.md'
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        for file in [self.out_h, self.out_h.with_name('midi_schema.h'),
                     self.out_js, self.out_md]:
            self.assertIn(str(file), result.stderr)
            self.assertFalse(file.parent.exists())

    def test_legacy_trailing_header_arguments_remain_supported(self):
        extras = [str(self.root / 'Effects' / 'tone.h')]
        self.assertEqual(self.run_generator(extra=extras).returncode, 0)
        before = self.snapshot()
        result = self.run_generator(check=True, extra=extras)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_line_endings_do_not_cause_false_staleness(self):
        for newline in (b'\n', b'\r\n'):
            with self.subTest(newline=newline):
                for output in self.outputs:
                    text = output.read_bytes().replace(b'\r\n', b'\n')
                    output.write_bytes(text.replace(b'\n', newline))
                before = self.snapshot()
                result = self.run_generator(check=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.snapshot(), before)

    def test_equal_basenames_in_separate_directories(self):
        self.out_js = self.out_js.with_name(self.out_h.name)
        self.out_md = self.out_md.with_name(self.out_h.name)
        self.outputs = [self.out_h, self.out_h.with_name('midi_schema.h'),
                        self.out_js, self.out_md]
        self.assertEqual(self.run_generator().returncode, 0)
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_directory_in_place_of_output_is_error(self):
        self.out_js.unlink()
        self.out_js.mkdir()
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue(self.out_js.is_dir())
        self.assertEqual(self.snapshot(), before)

    def test_missing_midi_input_is_error_without_output_changes(self):
        self.midi.unlink()
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_invalid_midi_constant_is_error(self):
        self.midi.write_text(self.midi.read_text().replace(
            'MIDI_CC_GLOBAL_ENABLE', 'REMOVED_GLOBAL_ENABLE'))
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('MIDI_CC_GLOBAL_ENABLE', result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_aliased_output_paths_are_rejected(self):
        self.out_js = self.out_h
        before = self.snapshot()
        result = self.run_generator(check=True)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.snapshot(), before)


if __name__ == '__main__':
    unittest.main()
