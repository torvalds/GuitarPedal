#!/usr/bin/env python3
import sys
import struct
import subprocess
import math
import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal.windows as windows
from scipy.fft import fft
import matplotlib.pyplot as plt
from matplotlib.widgets import Slider, Button

# Pedal's FFT settings from analyze.h
FFT_SHIFT = 13
FFT_SIZE = 1 << FFT_SHIFT

note_names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

def calculate_note_and_cents(freq):
    if freq <= 0.0:
        return 0, 0
    note_float = 69.0 + 12.0 * math.log2(freq / 440.0)
    note_idx = int(round(note_float))
    cents = int(round((note_float - note_idx) * 100.0))
    return note_idx, cents

def format_note(note_idx, cents):
    name_idx = note_idx % 12
    name = note_names[name_idx]
    octave = (note_idx // 12) - 1
    sign = "+" if cents >= 0 else ""
    return f"{name}{octave} {sign}{cents}c"

def extract_peak(mags, i, avg_mag, max_mag, min_mag=5.0):
    mag = mags[i]
    if mag < min_mag:
        return None
    if mag < 5.0 or mag < avg_mag * 5.0:
        return None
    if mag < max_mag * 0.05:
        return None
    if mag <= mags[i-1] or mag <= mags[i+1] or mag <= 1.2 * mags[i-2] or mag <= 1.2 * mags[i+2]:
        return None

    y_m2 = mags[i-2]
    y1 = mags[i-1]
    y2 = mag
    y3 = mags[i+1]
    y_p2 = mags[i+2]

    if y3 > y1:
        p = (2.0 * y3 - y2) / (y3 + y2)
    else:
        p = (y2 - 2.0 * y1) / (y1 + y2)

    peak_bin = i + p
    peak_freq = peak_bin * (12000.0 / FFT_SIZE)
    peak_mag = y2 - 0.25 * (y1 - y3) * p

    return peak_freq, peak_mag, peak_bin, i, y_m2, y1, y2, y3, y_p2

def get_peaks(mags, top_n=10):
    peaks = []
    avg_mag = np.mean(mags)
    max_mag = np.max(mags)

    for i in range(2, len(mags) - 2):
        res = extract_peak(mags, i, avg_mag, max_mag)
        if res is not None:
            peaks.append(res)

    peaks.sort(key=lambda x: x[1], reverse=True)
    return peaks[:top_n]

def suppress_harmonics(mags_input):
    mags = np.copy(mags_input)
    max_bin = len(mags)
    avg_mag = np.mean(mags)
    max_mag = np.max(mags)

    for i in range(2, max_bin // 2):
        res = extract_peak(mags, i, avg_mag, max_mag, min_mag=0.0)
        if res is None:
            continue

        peak_mag = res[1]
        peak_bin = res[2]

        h = 2
        while h * peak_bin < max_bin:
            target_exact = h * peak_bin
            low = int(math.floor(target_exact - 2.0))
            high = int(math.ceil(target_exact + 2.0))

            for j in range(low, high + 1):
                if j >= max_bin:
                    break
                if j < 0:
                    continue

                d = abs(j - target_exact)
                w = 0.0
                if d <= 1.0:
                    w = 1.0 - 0.5 * d * d
                elif d <= 2.0:
                    x = 2.0 - d
                    w = 0.5 * x * x

                suppression = peak_mag * w
                if mags[j] > suppression:
                    mags[j] -= suppression
                else:
                    mags[j] = 0.0
            h += 1

    return mags

def run_c_tuner(wav_file, offset):
    cmd = ['./test-fft', wav_file, str(offset * 4)]
    try:
        result = subprocess.run(cmd, capture_output=True, check=True)
        data = result.stdout

        header_format = '<II'
        header_size = struct.calcsize(header_format)

        if len(data) < header_size:
            print("Error: Output too short")
            return None, None

        out_offset, num_mags = struct.unpack(header_format, data[:header_size])

        if num_mags != FFT_SIZE // 2:
            print(f"Error: Unexpected number of magnitudes: {num_mags}")
            return None, None

        mags_format = f'<{num_mags}f'
        mags_size = struct.calcsize(mags_format)

        mags = struct.unpack(mags_format, data[header_size:header_size+mags_size])
        return out_offset, np.array(mags)

    except subprocess.CalledProcessError as e:
        print(f"C program failed with error code {e.returncode}")
        print("stderr:", e.stderr.decode('utf-8'))
        return None, None

current_peaks = {'raw': [], 'supp': []}

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <tuning.wav>")
        sys.exit(1)

    wav_file = sys.argv[1]

    try:
        sample_rate, audio_data = wavfile.read(wav_file)
    except Exception as e:
        print(f"Failed to read {wav_file}: {e}")
        sys.exit(1)

    if audio_data.dtype != np.int32:
        print(f"Warning: Audio data is {audio_data.dtype}, expected int32")

    if sample_rate != 48000:
        print(f"Warning: Sample rate is {sample_rate}, expected 48000")

    if len(audio_data.shape) > 1:
        audio_data = audio_data[:, 0]

    num_downsampled = len(audio_data) // 4
    downsampled_audio = audio_data[:num_downsampled*4].reshape(-1, 4).sum(axis=1)
    downsampled_audio = downsampled_audio / 2147483648.0

    hann_window = windows.hann(FFT_SIZE, sym=False)
    freqs = np.fft.fftfreq(FFT_SIZE, d=1.0/(sample_rate/4))
    freqs = freqs[:FFT_SIZE//2]

    # Pre-calculate Hann window for the non-downsampled audio (4x length)
    hann_window_full = windows.hann(FFT_SIZE * 4, sym=False)
    n_zp = FFT_SIZE * 4 * 16 # 16x zero-padding
    freqs_zp = np.fft.fftfreq(n_zp, d=1.0/sample_rate)
    freqs_zp = freqs_zp[:n_zp//2]
    max_zp_idx = int(1500.0 * n_zp / sample_rate) + 100

    fig, (ax_wav_full, ax_wav_zoom, ax_mag, ax_mag_supp) = plt.subplots(4, 1, figsize=(12, 12))
    plt.subplots_adjust(bottom=0.15, hspace=0.4)

    time_axis = np.arange(num_downsampled) / (sample_rate / 4)
    step = max(1, num_downsampled // 10000)
    ax_wav_full.plot(time_axis[::step], downsampled_audio[::step], color='lightgray', alpha=0.8)
    ax_wav_full.set_title("Full WAV File Amplitude (Downsampled)")
    ax_wav_full.set_ylabel("Amplitude")

    vline_start = ax_wav_full.axvline(0, color='red', linestyle='--')
    vline_end = ax_wav_full.axvline(FFT_SIZE / (sample_rate/4), color='red', linestyle='--')

    line_zoom, = ax_wav_zoom.plot(np.zeros(FFT_SIZE), np.zeros(FFT_SIZE), color='blue', alpha=0.7)
    ax_wav_zoom.set_title("Zoomed WAV Segment")
    ax_wav_zoom.set_ylabel("Amplitude")
    ax_wav_zoom.set_xlim(0, FFT_SIZE / (sample_rate/4))
    ax_wav_zoom.set_ylim(-1, 1)

    line_c, = ax_mag.plot(freqs, np.zeros_like(freqs), label="C test-fft", color='blue', alpha=0.7)
    line_py, = ax_mag.plot(freqs, np.zeros_like(freqs), label="SciPy Reference", color='orange', alpha=0.7, linestyle='--')
    line_zp, = ax_mag.plot(freqs_zp[:max_zp_idx], np.zeros(max_zp_idx), label="Zero-Padded (Full Rate)", color='green', alpha=0.5)

    ax_mag.set_title("FFT Magnitudes")
    ax_mag.set_ylabel("Magnitude")
    ax_mag.set_xlim(0, 1500)
    ax_mag.legend()
    ax_mag.grid(True)

    line_supp, = ax_mag_supp.plot(freqs, np.zeros_like(freqs), color='purple', alpha=0.8)
    ax_mag_supp.set_title("FFT Magnitudes (After Harmonic Suppression)")
    ax_mag_supp.set_ylabel("Magnitude")
    ax_mag_supp.set_xlabel("Frequency (Hz)")
    ax_mag_supp.set_xlim(0, 1500)
    ax_mag_supp.grid(True)

    error_text = fig.text(0.5, 0.95, '', ha='center', color='red', fontweight='bold')

    ax_slider = plt.axes([0.15, 0.05, 0.75, 0.03])
    max_offset = max(0, num_downsampled - FFT_SIZE)
    slider = Slider(ax_slider, 'Offset', 0, max_offset, valinit=0, valstep=128)

    peak_annotations_mag = []
    peak_annotations_supp = []

    def update(val):
        offset = int(slider.val)

        t_start = offset / (sample_rate / 4)
        t_end = (offset + FFT_SIZE) / (sample_rate / 4)
        vline_start.set_xdata([t_start, t_start])
        vline_end.set_xdata([t_end, t_end])

        segment = downsampled_audio[offset : offset + FFT_SIZE]
        if len(segment) < FFT_SIZE:
            return

        time_zoom = np.arange(FFT_SIZE) / (sample_rate / 4)
        line_zoom.set_xdata(time_zoom)
        line_zoom.set_ydata(segment)
        ax_wav_zoom.set_ylim(min(np.min(segment)*1.1, -0.01), max(np.max(segment)*1.1, 0.01))

        c_offset, c_mags = run_c_tuner(wav_file, offset)
        if c_mags is None:
            return

        windowed_data = segment * hann_window
        py_fft = fft(windowed_data)
        py_mags = np.abs(py_fft)[:FFT_SIZE//2]

        # Non-downsampled, zero-padded reference
        full_segment = audio_data[offset*4 : offset*4 + FFT_SIZE*4] / 2147483648.0
        full_windowed = full_segment * hann_window_full
        py_fft_zp = fft(full_windowed, n=n_zp)
        py_mags_zp = np.abs(py_fft_zp)[:n_zp//2]

        max_err = np.max(np.abs(c_mags - py_mags))
        if max_err > 0.1:
            error_text.set_text(f'WARNING: Max Error = {max_err:.4f}')
        else:
            error_text.set_text('')

        line_c.set_ydata(c_mags)
        line_py.set_ydata(py_mags)
        line_zp.set_ydata(py_mags_zp[:max_zp_idx])

        max_mag = max(np.max(c_mags), np.max(py_mags), np.max(py_mags_zp[:max_zp_idx]))
        if max_mag > 0:
            ax_mag.set_ylim(0, max_mag * 1.2)

        for text in peak_annotations_mag:
            text.remove()
        peak_annotations_mag.clear()

        peaks_raw = get_peaks(c_mags)
        enriched_peaks_raw = []
        for freq, mag, bin_idx, i, y_m2, y1, y2, y3, y_p2 in peaks_raw:
            # Find the true peak in the zero-padded data
            center_idx = int(freq * n_zp / sample_rate)
            search_window = int(2.0 * n_zp / sample_rate) # +/- 2 Hz
            w_start = max(0, center_idx - search_window)
            w_end = min(len(py_mags_zp), center_idx + search_window)

            zp_freq, zp_mag = freq, mag
            if w_end > w_start:
                local_max = w_start + np.argmax(py_mags_zp[w_start:w_end])
                zp_mag = py_mags_zp[local_max]

                # Standard parabolic interpolation on the dense zero-padded peak
                if local_max > 0 and local_max < len(py_mags_zp) - 1:
                    y1_zp = py_mags_zp[local_max - 1]
                    y2_zp = py_mags_zp[local_max]
                    y3_zp = py_mags_zp[local_max + 1]

                    denom = y1_zp - 2 * y2_zp + y3_zp
                    if denom != 0:
                        p_zp = 0.5 * (y1_zp - y3_zp) / denom
                    else:
                        p_zp = 0.0
                else:
                    p_zp = 0.0

                zp_freq = (local_max + p_zp) * (sample_rate / n_zp)

            enriched_peaks_raw.append((freq, mag, bin_idx, i, y_m2, y1, y2, y3, y_p2, zp_freq, zp_mag))

            note_idx, cents = calculate_note_and_cents(freq)
            note_str = format_note(note_idx, cents)
            text = ax_mag.annotate(f'{freq:.2f}Hz\n{note_str}',
                                   xy=(freq, mag),
                                   xytext=(0, 5),
                                   textcoords='offset points',
                                   ha='center', va='bottom',
                                   fontsize=8, color='darkblue',
                                   arrowprops=dict(arrowstyle='-', color='gray', lw=0.5))
            peak_annotations_mag.append(text)
        current_peaks['raw'] = enriched_peaks_raw

        # Whitening / Harmonic Suppression
        supp_mags = suppress_harmonics(c_mags)
        line_supp.set_ydata(supp_mags)

        max_supp_mag = np.max(supp_mags)
        if max_supp_mag > 0:
            ax_mag_supp.set_ylim(0, max_supp_mag * 1.2)

        for text in peak_annotations_supp:
            text.remove()
        peak_annotations_supp.clear()

        peaks_supp = get_peaks(supp_mags)
        current_peaks['supp'] = peaks_supp
        for freq, mag, bin_idx, i, y_m2, y1, y2, y3, y_p2 in peaks_supp:
            note_idx, cents = calculate_note_and_cents(freq)
            note_str = format_note(note_idx, cents)
            text = ax_mag_supp.annotate(f'{freq:.2f}Hz\n{note_str}',
                                   xy=(freq, mag),
                                   xytext=(0, 5),
                                   textcoords='offset points',
                                   ha='center', va='bottom',
                                   fontsize=8, color='purple',
                                   arrowprops=dict(arrowstyle='-', color='gray', lw=0.5))
            peak_annotations_supp.append(text)

        fig.canvas.draw_idle()

    slider.on_changed(update)

    step_size = FFT_SIZE // 16

    def on_key(event):
        if event.key == 'right':
            new_val = min(slider.val + step_size, max_offset)
            slider.set_val(new_val)
        elif event.key == 'left':
            new_val = max(slider.val - step_size, 0)
            slider.set_val(new_val)

    fig.canvas.mpl_connect('key_press_event', on_key)

    def on_click(event):
        if event.inaxes == ax_wav_full:
            center_sample = int(event.xdata * (sample_rate / 4))
            new_offset = center_sample - FFT_SIZE // 2
            new_val = max(0, min(new_offset, max_offset))
            slider.set_val(new_val)

    fig.canvas.mpl_connect('button_press_event', on_click)

    ax_btn = plt.axes([0.9, 0.05, 0.08, 0.03])
    btn = Button(ax_btn, 'Copy Peaks')

    def copy_peaks_cb(event):
        text = "=== PEAK DATA ===\n"
        text += "Raw Peaks:\n"
        for freq, mag, bin_idx, i, y_m2, y1, y2, y3, y_p2, zp_freq, zp_mag in current_peaks.get('raw', []):
            note_idx, cents = calculate_note_and_cents(freq)
            note_str = format_note(note_idx, cents)
            text += f"  - Freq: {freq:7.2f} Hz | Mag: {mag:6.2f} | Note: {note_str}\n"
            text += f"      [Bin {i:4d}] y-2: {y_m2:9.6f}, y-1: {y1:9.6f}, y0: {y2:9.6f}, y+1: {y3:9.6f}, y+2: {y_p2:9.6f}\n"
            text += f"      [Zero-Pad] Freq: {zp_freq:7.2f} Hz | Mag: {zp_mag:6.2f} | Diff: {freq - zp_freq:6.2f} Hz\n"

        text += "\nSuppressed Peaks:\n"
        for freq, mag, bin_idx, i, y_m2, y1, y2, y3, y_p2 in current_peaks.get('supp', []):
            note_idx, cents = calculate_note_and_cents(freq)
            note_str = format_note(note_idx, cents)
            text += f"  - Freq: {freq:7.2f} Hz | Mag: {mag:6.2f} | Note: {note_str}\n"
            text += f"      [Bin {i:4d}] y-2: {y_m2:9.6f}, y-1: {y1:9.6f}, y0: {y2:9.6f}, y+1: {y3:9.6f}, y+2: {y_p2:9.6f}\n"

        text += "=================\n"
        print(text)

        try:
            import tkinter as tk
            r = tk.Tk()
            r.withdraw()
            r.clipboard_clear()
            r.clipboard_append(text)
            r.update()
            r.destroy()
            print("(Also copied to clipboard!)")
        except Exception:
            print("(Could not copy to clipboard automatically, please copy from the terminal)")

    btn.on_clicked(copy_peaks_cb)

    update(0)
    plt.show()

if __name__ == "__main__":
    main()
