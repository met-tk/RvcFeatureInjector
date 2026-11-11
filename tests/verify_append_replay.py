"""Real RVC: identical mid-vowel/fricative Append and playback from the edit cursor."""
import argparse
import ctypes
import json
import os
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--speech-wav", type=Path, required=True)
parser.add_argument("--output", type=Path, default=ROOT / "artifacts/append-cache-1.1.8/neural")
args = parser.parse_args()
assert all(os.environ.get("RVC_TEST_MODEL_"+slot) for slot in ("A","B"))
sr = 44100
args.output.mkdir(parents=True, exist_ok=True)
rate, speech = wavfile.read(args.speech_wav)
if np.issubdtype(speech.dtype,np.integer):
    speech = speech.astype(np.float32)/np.iinfo(speech.dtype).max
if speech.ndim == 2:
    speech = speech.mean(axis=1)
speech = signal.resample_poly(speech,sr,rate).astype(np.float32)
assert np.isfinite(speech).all() and np.max(np.abs(speech)) > .01
speech *= .4/max(float(np.max(np.abs(speech))),1e-8)
t = np.arange(6*sr,dtype=np.float64)/sr
vowel = sum(.12/h*np.sin(2*np.pi*220*h*t) for h in range(1,13)).astype(np.float32)
# Select an interior high-frequency speech window, rather than assuming a consonant timestamp.
high = signal.sosfilt(signal.butter(4,3500,fs=sr,btype="highpass",output="sos"),speech)
n = sr//50
energy = np.convolve(high*high,np.ones(n)/n,mode="same")
energy[:sr] = energy[-sr:] = 0
punch = int(np.argmax(energy))
assert sr < punch < len(speech)-sr
library = ctypes.WinDLL(str(ROOT / "dist/RvcFeatureInjector.vst3/Contents/x86_64-win/RvcFeatureInjector.vst3"))
verify = library.RvcVst3_VerifyAppendReplay
verify.argtypes = [ctypes.POINTER(ctypes.c_float),ctypes.c_uint32,ctypes.c_uint32,ctypes.c_wchar_p,
                   ctypes.POINTER(ctypes.c_float)]
verify.restype = ctypes.c_bool
results = []
for name, source, edit in (("vowel",vowel,3*sr+37),("fricative",speech,punch)):
    out = args.output/name
    out.mkdir(exist_ok=True)
    source = np.ascontiguousarray(source,dtype=np.float32)
    metrics = (ctypes.c_float*5)()
    success = verify(source.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),len(source),edit,str(out),metrics)
    row = {"case":name,"passed":success,"sample_rate":sr,"buffer_samples":128,"edit_sample":edit,
           "reused_cache":metrics[0]==1,"changed_input_samples":metrics[1],
           "whole_render_max_difference":metrics[2],"live_input_max_difference":metrics[3],
           "middle_start_max_difference":metrics[4],"rms_settings":[0,.25,1]}
    results.append(row)
    (args.output/"results.json").write_text(json.dumps(results,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(row),flush=True)
    assert success,row
    wavfile.write(out/"source.wav",sr,source)
    for stem in ("baseline-wet","after-append-wet"):
        wavfile.write(out/(stem+".wav"),sr,np.fromfile(out/(stem+".f32"),dtype=np.float32))
