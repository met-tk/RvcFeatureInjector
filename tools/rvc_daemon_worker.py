#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RVC Native Neural Synthesis Daemon Worker (Scheme A: HuBERT + RMVPE + FAISS + VITS/NSF-HiFiGAN)

Communicates with the C++ VST3 plugin (RvcModelEngine) over binary anonymous pipes:
- Redirects all library stdout logs to stderr at startup so binary frames on ipc_out are never corrupted.
- Loads HuBERT Base (768D/256D) and RMVPE pitch extractor onto CUDA (FP16/FP32) or CPU fallback.
- Supports dual-slot RVC v1/v2 .pth + FAISS .index models (including Unicode/Chinese paths on Windows).
- Caches (feats_0, f0_base_hz) per timeline audio chunk ID so knob changes (A/B Morph, Pitch,
  Formant, Index Rate, Protect, Inject Depth, Clarity) re-synthesize in ~8-15ms on GPU.
- Streams real-time MSG_PROGRESS updates to drive the VST3 UI progress bar.
"""

import os
import sys
import time
import math
import struct
import hashlib
import traceback
import argparse
import re
import tempfile
from collections import OrderedDict

# ---------------------------------------------------------------------------
# 1. Isolate Binary Pipe FD BEFORE importing third-party libraries
# ---------------------------------------------------------------------------
IS_TEST_CLI = ("--test-cli" in sys.argv)

# GUI hosts (Cubase) may spawn us without a usable stderr; never run with sys.stderr == None
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w", encoding="utf-8")

if not IS_TEST_CLI:
    import msvcrt
    # Binary mode first, unconditionally, so 0x0A is never expanded to 0x0D 0x0A
    msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
    msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
    IPC_IN = sys.stdin.buffer
    try:
        ipc_out_fd = os.dup(sys.stdout.fileno())
        msvcrt.setmode(ipc_out_fd, os.O_BINARY)
        try:
            os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
        except (OSError, AttributeError, ValueError):
            # No real stderr fd: point fd 1 at NUL so stray library prints cannot reach the pipe
            nul_fd = os.open(os.devnull, os.O_WRONLY)
            os.dup2(nul_fd, sys.stdout.fileno())
            os.close(nul_fd)
        sys.stdout = sys.stderr
        IPC_OUT = os.fdopen(ipc_out_fd, "wb", buffering=0)
    except OSError:
        IPC_OUT = sys.stdout.buffer
else:
    IPC_IN = None
    IPC_OUT = None

# Use the same runtime for imports and assets; an explicit missing root must fail
# rather than silently combining libraries/assets from another installation.
_args = argparse.ArgumentParser(add_help=False)
_args.add_argument("--runtime-root")
_args.add_argument("--state-root")
_args.add_argument("--model-a")
_args.add_argument("--model-b")
_args.add_argument("--index-a", default="")
_args.add_argument("--index-b", default="")
_runtime_args, _ = _args.parse_known_args()
RVC_RUNTIME_ROOT = os.path.abspath(
    _runtime_args.runtime_root or os.environ.get("RVC_RUNTIME_ROOT") or r"D:\RVC20260718Nvidia"
)
if RVC_RUNTIME_ROOT not in sys.path:
    sys.path.insert(0, RVC_RUNTIME_ROOT)

def configure_runtime_state(state_root):
    """Keep an installed worker's generated files inside its uninstallable state folder."""
    if not state_root:
        # Portable users can keep their existing cache configuration.
        numba_cache = os.environ.setdefault(
            "NUMBA_CACHE_DIR", os.path.join(tempfile.gettempdir(), "RvcFeatureInjector", "numba")
        )
        if numba_cache:
            os.makedirs(numba_cache, exist_ok=True)
        return None
    if not os.path.isabs(state_root):
        raise ValueError("--state-root must be an absolute path")
    state_root = os.path.abspath(state_root)
    paths = {
        "TEMP": "temp", "TMP": "temp", "TMPDIR": "temp", "JOBLIB_TEMP_FOLDER": "temp",
        "XDG_CACHE_HOME": "cache", "NUMBA_CACHE_DIR": "cache/numba",
        "TORCH_HOME": "cache/torch", "TORCH_EXTENSIONS_DIR": "cache/torch/extensions",
        "TORCHINDUCTOR_CACHE_DIR": "cache/torch/inductor",
        "PYTORCH_KERNEL_CACHE_PATH": "cache/torch/kernels", "TRITON_CACHE_DIR": "cache/triton",
        "CUDA_CACHE_PATH": "cache/cuda", "HF_HOME": "cache/huggingface",
        "HF_HUB_CACHE": "cache/huggingface/hub", "HUGGINGFACE_HUB_CACHE": "cache/huggingface/hub",
        "TRANSFORMERS_CACHE": "cache/huggingface/hub", "HF_ASSETS_CACHE": "cache/huggingface/assets",
        "HUGGINGFACE_ASSETS_CACHE": "cache/huggingface/assets", "HF_XET_CACHE": "cache/huggingface/xet",
        "LIBROSA_CACHE_DIR": "cache/librosa", "MPLCONFIGDIR": "cache/matplotlib",
    }
    for variable, relative in paths.items():
        path = os.path.join(state_root, *relative.split("/"))
        os.makedirs(path, exist_ok=True)
        os.environ[variable] = path
    tempfile.tempdir = os.environ["TEMP"]
    sys.dont_write_bytecode = True
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
    return state_root


MANAGED_STATE_ROOT = configure_runtime_state(_runtime_args.state_root)

import warnings
warnings.filterwarnings("ignore")

import numpy as np
import scipy.signal as signal
import torch
import torch.nn.functional as F

# 5th-order 48Hz Butterworth high-pass filter @ 16kHz from D:\RVC20260718Nvidia\infer\vc\pipeline.py
HIGHPASS_BH, HIGHPASS_AH = signal.butter(N=5, Wn=48, btype="high", fs=16000)

try:
    import faiss
    HAS_FAISS = True
except Exception:
    HAS_FAISS = False

# ---------------------------------------------------------------------------
# 2. Binary Protocol Constants
# ---------------------------------------------------------------------------
REQ_MAGIC = b"RVCQ"
RESP_MAGIC = b"RVCR"

CMD_INIT = 1
CMD_LOAD_SLOT = 2
CMD_LOAD_PRESET = 3
CMD_CLEAR_SLOT = 4
CMD_SYNTH_CHUNK = 5
CMD_SHUTDOWN = 99

MSG_PROGRESS = 10
MSG_STATUS_OK = 20
MSG_SLOT_INFO = 21
MSG_SYNTH_RESULT = 22
MSG_ERROR = 90

def send_packet(msg_type: int, payload: bytes = b""):
    if IPC_OUT is None:
        return
    header = struct.pack("<4sII", RESP_MAGIC, msg_type, len(payload))
    try:
        IPC_OUT.write(header + payload)
        IPC_OUT.flush()
    except (OSError, BrokenPipeError):
        pass


def send_progress(progress: float, status_text: str):
    p_clamped = float(max(0.0, min(1.0, progress)))
    if IS_TEST_CLI:
        print(f"[PROGRESS {p_clamped * 100.0:5.1f}%] {status_text}", file=sys.stderr)
        return
    text_bytes = status_text.encode("utf-8")[:240]
    payload = struct.pack("<fI", p_clamped, len(text_bytes)) + text_bytes
    send_packet(MSG_PROGRESS, payload)


def send_error(err_text: str):
    if IS_TEST_CLI:
        print(f"[ERROR] {err_text}", file=sys.stderr)
        return
    text_bytes = err_text.encode("utf-8")[:500]
    payload = struct.pack("<I", len(text_bytes)) + text_bytes
    send_packet(MSG_ERROR, payload)


def read_exact(stream, num_bytes: int) -> bytes:
    buf = bytearray()
    while len(buf) < num_bytes:
        chunk = stream.read(num_bytes - len(buf))
        if not chunk:
            return b""
        buf.extend(chunk)
    return bytes(buf)


# ---------------------------------------------------------------------------
# 3. Companion File Discovery & Spectral Signature Helpers
# ---------------------------------------------------------------------------
def resolve_pth_and_index(user_path: str, explicit_index: str = None):
    """
    Given a user-selected .pth or .index file path, locates both the .pth
    checkpoint and the companion .index file in the same directory.
    """
    if not user_path or not os.path.isfile(user_path):
        raise FileNotFoundError(f"Model file not found: {user_path}")

    ext = os.path.splitext(user_path)[1].lower()
    folder = os.path.dirname(user_path)

    def companion(extension, selected_stem):
        candidates = sorted(
            os.path.join(folder, f) for f in os.listdir(folder)
            if f.lower().endswith(extension)
        )
        matches = [p for p in candidates if (
            selected_stem.lower() in os.path.splitext(os.path.basename(p))[0].lower()
            or (extension == ".pth" and os.path.splitext(os.path.basename(p))[0].lower() in selected_stem.lower())
        )]
        if len(matches) == 1:
            return matches[0]
        candidates = matches or candidates
        if extension == ".index":
            # Prefer populated added indices within the matching model family.
            added = [p for p in candidates if os.path.basename(p).lower().startswith("added")]
            candidates = added or candidates
        if len(candidates) > 1:
            raise ValueError(f"Ambiguous {extension} companion for {user_path}; select an explicit model/index pair")
        return candidates[0] if candidates else ""

    pth_path, index_path = "", ""

    if ext == ".pth":
        pth_path = user_path
        index_path = companion(".index", os.path.splitext(os.path.basename(user_path))[0]) if explicit_index is None else explicit_index
    elif ext == ".index":
        index_path = user_path
        pth_path = companion(".pth", os.path.splitext(os.path.basename(user_path))[0])
        if explicit_index is not None:
            index_path = explicit_index
    else:
        raise ValueError(f"Expected an RVC .pth or .index file: {user_path}")
    if not pth_path:
        raise FileNotFoundError(f"No RVC .pth checkpoint accompanies {user_path}")
    if index_path and (not os.path.isfile(index_path) or os.path.splitext(index_path)[1].lower() != ".index"):
        raise FileNotFoundError(f"FAISS index not found: {index_path}")
    return pth_path, index_path


def select_device_precision():
    """Match the reference runtime's CUDA eligibility without importing Config side effects."""
    profiles = []
    if torch.cuda.is_available():
        for idx in range(torch.cuda.device_count()):
            try:
                sm = torch.cuda.get_device_capability(idx)
                memory_gb = torch.cuda.get_device_properties(idx).total_memory / (1024 ** 3) + 0.4
                name = torch.cuda.get_device_name(idx)
                if memory_gb < 4 or sm < (5, 3):
                    continue
                is_16_series = sm == (7, 5) and bool(re.search(r"16\d{2}", name))
                if sm == (6, 1) or is_16_series:
                    profiles.append((sm, memory_gb, idx, False))
                elif sm > (6, 1):
                    profiles.append((sm, memory_gb, idx, True))
            except Exception:
                continue
    if not profiles:
        return torch.device("cpu"), False
    _, _, idx, is_half = max(profiles)
    return torch.device(f"cuda:{idx}"), is_half


def sample_automation_curve(curves, column, total_frames, hop_16k, left_pad_16k):
    """Curves start at submitted audio sample 0 on a fixed 160-sample (10 ms) grid."""
    source_samples = np.arange(curves.shape[0], dtype=np.float64) * 160
    feature_samples = np.arange(total_frames, dtype=np.float64) * hop_16k - left_pad_16k
    return np.interp(feature_samples, source_samples, curves[:, column]).astype(np.float32)


def unpack_load_slot_payload(payload):
    if len(payload) < 8:
        raise ValueError("Truncated LOAD_SLOT header")
    slot_idx, path_len = struct.unpack_from("<II", payload)
    end = 8 + path_len
    if end > len(payload):
        raise ValueError("Truncated checkpoint path")
    path_str = payload[8:end].decode("utf-8")
    index_path = None  # Legacy clients requested automatic companion discovery.
    if end < len(payload):
        if len(payload) < end + 4:
            raise ValueError("Truncated index path length")
        (index_len,) = struct.unpack_from("<I", payload, end)
        if end + 4 + index_len != len(payload):
            raise ValueError("Invalid index path length")
        index_path = payload[end + 4:].decode("utf-8")
    return slot_idx, path_str, index_path


def compute_128_signature_from_index(big_npy: np.ndarray, seed_val: int = 0) -> np.ndarray:
    """
    Computes a deterministic 128-point formant signature curve (-10..+10 dB)
    from the loaded RVC FAISS index centroids for the Slot UI sparkline.
    """
    if big_npy is not None and big_npy.ndim == 2 and big_npy.shape[0] > 0:
        step = max(1, big_npy.shape[0] // 256)
        mean_vec = np.mean(big_npy[::step], axis=0).astype(np.float32)
        # Project 768/256 dims onto 128 smooth bins
        x_old = np.linspace(0.0, 1.0, len(mean_vec), dtype=np.float32)
        x_new = np.linspace(0.0, 1.0, 128, dtype=np.float32)
        curve = np.interp(x_new, x_old, mean_vec).astype(np.float32)
        curve -= np.mean(curve)
        std = float(np.std(curve))
        if std > 1e-5:
            curve = (curve / std) * 4.2
        # Smooth with a 5-tap Hann kernel
        kernel = np.array([0.1, 0.2, 0.4, 0.2, 0.1], dtype=np.float32)
        curve = np.convolve(curve, kernel, mode="same")
        return np.clip(curve, -10.0, 10.0).astype(np.float32)

    t = np.linspace(0.0, 1.0, 128, dtype=np.float32)
    phase = float(seed_val % 7) * 0.6
    curve = 4.0 * np.sin(2.0 * np.pi * 2.5 * t + phase) + 2.5 * np.cos(2.0 * np.pi * 5.0 * t)
    return curve.astype(np.float32)


def compute_64bin_scope_envelope(wav: np.ndarray, sr: int) -> np.ndarray:
    """
    Computes a smooth 64-bin log-frequency spectral envelope (55 Hz .. 14.3 kHz)
    in dB (-52 .. +6 dB) matching the Direct2D UI oscilloscope scale.
    """
    if wav is None or len(wav) < 256:
        return np.full(64, -48.0, dtype=np.float32)

    n_fft = 1024
    hop = 512
    # Take middle active portion of the chunk
    seg = wav
    if len(seg) > sr * 2:
        mid = len(seg) // 2
        seg = seg[mid - sr : mid + sr]

    _, _, zxx = signal.stft(seg, fs=sr, window="hann", nperseg=n_fft, noverlap=n_fft - hop)
    mag = np.abs(zxx)
    frame_energy = np.mean(mag ** 2, axis=0)
    active_mask = frame_energy > (np.max(frame_energy) * 0.05 + 1e-9)
    if np.any(active_mask):
        mean_mag = np.mean(mag[:, active_mask], axis=1)
    else:
        mean_mag = np.mean(mag, axis=1)

    # Cepstral smoothing of log magnitude
    log_mag = np.log(np.maximum(mean_mag * 2.0, 1e-4))
    cep = np.fft.irfft(log_mag)
    lifter = np.zeros_like(cep)
    cutoff = 28
    lifter[:cutoff] = 1.0
    lifter[-cutoff + 1 :] = 1.0
    smooth_ln = np.fft.rfft(cep * lifter).real
    smooth_db = smooth_ln * 8.6858896 + 12.0

    freqs = np.linspace(0.0, sr / 2.0, len(smooth_db), dtype=np.float32)
    s_idx = np.linspace(0.0, 1.0, 64, dtype=np.float32)
    target_freqs = 55.0 * (260.0 ** s_idx)
    env_64 = np.interp(target_freqs, freqs, smooth_db).astype(np.float32)
    return np.clip(env_64, -52.0, 6.0)


# ---------------------------------------------------------------------------
# 4. RVC Slot & Daemon Engine
# ---------------------------------------------------------------------------
class RvcSlotModel:
    def __init__(self, slot_idx: int):
        self.slot_idx = slot_idx
        self.is_loaded = False
        self.has_pth = False
        self.has_index = False
        self.model_name = "UNLOADED"
        self.file_path = ""
        self.index_path = ""
        self.index_identity = None
        self.arch_tag = "NONE"
        self.sr = 40000
        self.if_f0 = 1
        self.version = "v2"
        self.feature_dim = 768
        self.vector_count = 0
        self.net_g = None
        self.index = None
        self.big_npy = None
        self.base_formant_db = np.zeros(128, dtype=np.float32)


class RvcDaemonEngine:
    def __init__(self):
        self.device, self.is_half = select_device_precision()
        self.hubert = None
        self.rmvpe = None
        self.base_initialized = False

        self.slots = [RvcSlotModel(0), RvcSlotModel(1)]

        # LRU cache of extracted (feats_768_np, feats_256_np, f0_base_hz_np, dry_env_64, retrieved_map) keyed by audio SHA1 digest
        # Allows instant (<35ms) re-synthesis when user adjusts any knob or switches RVC models!
        self.feature_cache = OrderedDict()
        self.max_cache_entries = 64

        self.f0_min = 50.0
        self.f0_max = 1100.0
        self.f0_mel_min = 1127.0 * math.log(1.0 + self.f0_min / 700.0)
        self.f0_mel_max = 1127.0 * math.log(1.0 + self.f0_max / 700.0)

    def ensure_base_models(self):
        if self.base_initialized and self.hubert is not None and self.rmvpe is not None:
            return True

        send_progress(0.08, "LOADING HUBERT BASE (768D) ON CUDA...")
        hubert_dir = os.path.join(RVC_RUNTIME_ROOT, "assets", "hubert_base")
        rmvpe_path = os.path.join(RVC_RUNTIME_ROOT, "assets", "rmvpe", "rmvpe.pt")

        if not os.path.isdir(hubert_dir) or not os.path.exists(rmvpe_path):
            raise FileNotFoundError(f"Missing RVC base assets at {hubert_dir} or {rmvpe_path}")

        from infer.hubert import HubertModelWithFinalProj, hubert_audio_requires_normalization
        from infer.rmvpe import RMVPE

        self.hubert_norm = bool(hubert_audio_requires_normalization())
        self.hubert = HubertModelWithFinalProj.from_pretrained(hubert_dir)
        self.hubert = self.hubert.to(self.device)
        if self.is_half:
            self.hubert = self.hubert.half()
        else:
            self.hubert = self.hubert.float()
        self.hubert.eval()

        send_progress(0.45, "LOADING RMVPE PITCH EXTRACTOR ON CUDA...")
        self.rmvpe = RMVPE(rmvpe_path, is_half=self.is_half, device=self.device)

        # Warm up CUDA kernels with a short 0.25s dummy tensor
        send_progress(0.78, "WARMING UP CUDA NEURAL PIPELINE...")
        with torch.no_grad():
            dummy_wav = np.zeros(4000, dtype=np.float32)
            dummy_t = torch.from_numpy(dummy_wav).to(self.device).unsqueeze(0)
            if self.is_half:
                dummy_t = dummy_t.half()
            _ = self.hubert(dummy_t)["last_hidden_state"]
            _ = self.rmvpe.infer_from_audio(dummy_wav, thred=0.03)

        self.base_initialized = True
        send_progress(1.0, "RVC CUDA ENGINE READY")
        return True

    def load_slot(self, slot_idx: int, path_str: str, index_path: str = None) -> RvcSlotModel:
        if slot_idx not in (0, 1):
            raise ValueError(f"Invalid slot index {slot_idx}")

        slot_label = "SLOT A" if slot_idx == 0 else "SLOT B"
        send_progress(0.12, f"RESOLVING {slot_label} MODEL FILES...")

        pth_path, index_path = resolve_pth_and_index(path_str, index_path)
        self.ensure_base_models()

        new_slot = RvcSlotModel(slot_idx)
        new_slot.file_path = pth_path if pth_path else index_path
        stem = os.path.splitext(os.path.basename(new_slot.file_path))[0]
        new_slot.model_name = stem

        if pth_path and os.path.exists(pth_path):
            send_progress(0.35, f"LOADING {slot_label} VITS-NSF CHECKPOINT ({stem})...")
            from infer.module.models import (
                SynthesizerTrnMs256NSFsid,
                SynthesizerTrnMs256NSFsid_nono,
                SynthesizerTrnMs768NSFsid,
                SynthesizerTrnMs768NSFsid_nono,
            )

            cpt = torch.load(pth_path, map_location="cpu")
            config = list(cpt["config"])
            config[-3] = cpt["weight"]["emb_g.weight"].shape[0]
            if_f0 = cpt.get("f0", 1)
            version = cpt.get("version", "v1")
            if version not in ("v1", "v2") or if_f0 not in (0, 1):
                raise ValueError(f"Unsupported RVC checkpoint version/f0: {version}/{if_f0}")
            sr_val = config[-1]
            if isinstance(sr_val, str):
                sr_map = {"32k": 32000, "40k": 40000, "48k": 48000}
                sr_val = sr_map.get(sr_val, 40000)

            if version == "v1":
                if if_f0 == 1:
                    net_g = SynthesizerTrnMs256NSFsid(*config, is_half=self.is_half)
                else:
                    net_g = SynthesizerTrnMs256NSFsid_nono(*config)
                feat_dim = 256
            else:
                if if_f0 == 1:
                    net_g = SynthesizerTrnMs768NSFsid(*config, is_half=self.is_half)
                else:
                    net_g = SynthesizerTrnMs768NSFsid_nono(*config)
                feat_dim = 768

            if hasattr(net_g, "enc_q"):
                del net_g.enc_q

            net_g.load_state_dict(cpt["weight"], strict=False)
            net_g.eval().to(self.device)
            if self.is_half:
                net_g = net_g.half()
            else:
                net_g = net_g.float()
            net_g.remove_weight_norm()

            new_slot.net_g = net_g
            new_slot.has_pth = True
            new_slot.sr = int(sr_val)
            new_slot.if_f0 = int(if_f0)
            new_slot.version = str(version)
            new_slot.feature_dim = feat_dim

        if index_path:
            if not HAS_FAISS:
                raise RuntimeError("FAISS is unavailable; cannot load the requested index")
            send_progress(0.72, f"LOADING {slot_label} FAISS INDEX ({os.path.basename(index_path)})...")
            # np.fromfile + deserialize_index supports Unicode paths on Windows.
            raw_bytes = np.fromfile(index_path, dtype=np.uint8)
            index = faiss.deserialize_index(raw_bytes)
            if int(index.d) != new_slot.feature_dim:
                raise ValueError(f"Index dimension {index.d} does not match {new_slot.version} model dimension {new_slot.feature_dim}")
            if not index.is_trained or int(index.ntotal) <= 0:
                raise ValueError(f"Index has no trained retrieval vectors: {index_path}")
            if index.metric_type != faiss.METRIC_L2:
                raise ValueError(f"RVC retrieval requires a squared-L2 index: {index_path}")
            # IVF reconstruction may require a direct map.
            try:
                ivf = faiss.extract_index_ivf(index)
                if ivf is not None and getattr(ivf, "direct_map", None) is not None \
                        and ivf.direct_map.type == faiss.DirectMap.NoMap:
                    ivf.make_direct_map()
            except Exception:
                pass
            big_npy = index.reconstruct_n(0, index.ntotal)
            if big_npy.shape != (int(index.ntotal), new_slot.feature_dim) or not np.isfinite(big_npy).all():
                raise ValueError(f"Index contains invalid retrieval vectors: {index_path}")
            new_slot.index = index
            new_slot.big_npy = big_npy
            new_slot.has_index = True
            new_slot.index_path = os.path.abspath(index_path)
            new_slot.index_identity = (os.path.normcase(new_slot.index_path), hashlib.sha256(raw_bytes).digest())
            new_slot.vector_count = int(index.ntotal)

        new_slot.is_loaded = new_slot.has_pth
        new_slot.base_formant_db = compute_128_signature_from_index(
            new_slot.big_npy, seed_val=hash(new_slot.model_name)
        )

        sr_khz = f"{new_slot.sr // 1000}K"
        ver_str = new_slot.version.upper()
        if new_slot.has_pth and new_slot.has_index:
            new_slot.arch_tag = f"RVC-{ver_str} {sr_khz} / {new_slot.feature_dim}D"
        elif new_slot.has_pth:
            new_slot.arch_tag = f"RVC-{ver_str} {sr_khz} (NO-IDX)"
        else:
            new_slot.arch_tag = f"FAISS-IDX / {new_slot.feature_dim}D"

        self.slots[slot_idx] = new_slot
        send_progress(1.0, f"{slot_label} READY: {new_slot.model_name} ({sr_khz})")
        return new_slot

    def load_preset(self, slot_idx: int, preset_idx: int) -> RvcSlotModel:
        raise ValueError("Built-in singer presets are unavailable; import a .pth model and optional .index file manually.")

    def clear_slot(self, slot_idx: int) -> RvcSlotModel:
        if slot_idx in (0, 1):
            self.slots[slot_idx] = RvcSlotModel(slot_idx)
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
        return self.slots[slot_idx]

    def pack_slot_info(self, slot: RvcSlotModel) -> bytes:
        name_b = slot.model_name.encode("utf-8")[:63]
        path_b = slot.file_path.encode("utf-8")[:511]
        arch_b = slot.arch_tag.encode("utf-8")[:63]
        index_b = slot.index_path.encode("utf-8")[:511]

        name_fixed = name_b + b"\x00" * (64 - len(name_b))
        path_fixed = path_b + b"\x00" * (512 - len(path_b))
        arch_fixed = arch_b + b"\x00" * (64 - len(arch_b))

        hdr = struct.pack(
            "<IIIIIIII",
            int(slot.slot_idx),
            1 if slot.is_loaded else 0,
            1 if slot.has_pth else 0,
            1 if slot.has_index else 0,
            int(slot.sr),
            2 if slot.version == "v2" else 1,
            int(slot.vector_count),
            int(slot.feature_dim),
        )
        sig_bytes = slot.base_formant_db.astype(np.float32).tobytes()
        return hdr + name_fixed + path_fixed + arch_fixed + sig_bytes + index_b + b"\x00" * (512 - len(index_b))

    def _extract_or_get_cached_features(self, audio_16k: np.ndarray, progress_base: float, progress_span: float):
        """
        Extracts or retrieves cached (feats_768_np, feats_256_np, f0_hz_np, dry_env_64)
        for a 16kHz mono vocal audio chunk.
        """
        # Compute fast digest over audio_16k
        digest = hashlib.sha1(audio_16k.tobytes()).digest()
        if digest in self.feature_cache:
            self.feature_cache.move_to_end(digest)
            send_progress(
                progress_base + progress_span * 0.35,
                "CACHE HIT: REUSING HUBERT & RMVPE FEATURES...",
            )
            return self.feature_cache[digest], True

        self.ensure_base_models()

        send_progress(progress_base + progress_span * 0.10, "EXTRACTING HUBERT CONTENT FEATURES...")
        with torch.no_grad():
            wav_t = torch.from_numpy(audio_16k).to(self.device).unsqueeze(0)
            if getattr(self, "hubert_norm", False):
                wav_t = F.layer_norm(wav_t, wav_t.shape)
            if self.is_half:
                wav_t = wav_t.half()
            else:
                wav_t = wav_t.float()

            # Authentic RVC v1 (layer 9 + final_proj -> 256D) & v2 (layer 12 -> 768D)
            # per D:\RVC20260718Nvidia\infer\hubert.py
            hubert_out = self.hubert(
                input_values=wav_t,
                attention_mask=None,
                output_hidden_states=True,
                return_dict=True,
            )
            feats_768_t = hubert_out.last_hidden_state
            if hasattr(self.hubert, "final_proj") and hubert_out.hidden_states is not None and len(hubert_out.hidden_states) > 9:
                feats_256_t = self.hubert.final_proj(hubert_out.hidden_states[9])
            else:
                feats_256_t = feats_768_t[:, :, :256]

            feats_768 = feats_768_t.squeeze(0).float().cpu().numpy()
            feats_256 = feats_256_t.squeeze(0).float().cpu().numpy()

        send_progress(progress_base + progress_span * 0.28, "EXTRACTING RMVPE NEURAL PITCH F0...")
        with torch.no_grad():
            f0_hz = self.rmvpe.infer_from_audio(audio_16k, thred=0.045).astype(np.float32)

        # Preserve RMVPE's voiced/unvoiced decision, including brief notes and
        # bright high vowels whose harmonics can have a very high crossing rate.

        dry_env_64 = compute_64bin_scope_envelope(audio_16k, 16000)

        entry = (feats_768, feats_256, f0_hz, dry_env_64, {})
        self.feature_cache[digest] = entry
        if len(self.feature_cache) > self.max_cache_entries:
            self.feature_cache.popitem(last=False)

        return entry, False

    def _retrieve_slot_features(
        self,
        slot: RvcSlotModel,
        base_feats_np: np.ndarray,
        global_index_rate: float,
        slot_index_rate: float,
        retrieved_cache: dict = None,
        slot_index_curve_50fps: np.ndarray = None,
    ) -> np.ndarray:
        """
        Performs k-NN FAISS retrieval in 768D/256D HuBERT space (cached per segment + index identity)
        and blends retrieved timbre vectors using either scalar or per-frame (automation) index_rate.
        """
        if not slot.has_index or slot.index is None or slot.big_npy is None:
            return base_feats_np.copy()

        g_rate = float(max(0.0, min(1.0, global_index_rate)))
        s_rate = float(max(0.0, min(1.0, slot_index_rate)))
        max_possible = g_rate * (
            float(np.max(slot_index_curve_50fps)) if slot_index_curve_50fps is not None else s_rate
        )
        if max_possible <= 0.001:
            return base_feats_np.copy()

        npy = base_feats_np.astype(np.float32)
        if npy.shape[1] != slot.big_npy.shape[1]:
            raise ValueError("Loaded model/index feature dimensions no longer match")

        cache_key = slot.index_identity
        if retrieved_cache is not None and cache_key in retrieved_cache:
            retrieved = retrieved_cache[cache_key]
        else:
            score, ix = slot.index.search(npy, k=8)
            # FAISS returns -1 for missing neighbours; mask them instead of indexing the last vector
            valid = (ix >= 0) & (ix < slot.big_npy.shape[0]) & np.isfinite(score)
            ix_safe = np.where(valid, ix, 0)
            distance = np.where(valid, score, 1.0)
            weight = np.square(1.0 / np.maximum(distance, 1e-6)).astype(np.float32) * valid.astype(np.float32)
            weight /= np.maximum(np.sum(weight, axis=1, keepdims=True), 1e-8)
            retrieved = np.sum(slot.big_npy[ix_safe] * np.expand_dims(weight, axis=2), axis=1).astype(np.float32)
            no_hit = ~valid.any(axis=1)
            if np.any(no_hit):
                retrieved[no_hit] = npy[no_hit]
            if retrieved_cache is not None and cache_key:
                retrieved_cache[cache_key] = retrieved

        if slot_index_curve_50fps is not None and len(slot_index_curve_50fps) == npy.shape[0]:
            eff_vec = np.clip(slot_index_curve_50fps * g_rate, 0.0, 1.0).astype(np.float32)[:, None]
            return eff_vec * retrieved + (1.0 - eff_vec) * npy

        eff_rate = float(max(0.0, min(1.0, g_rate * s_rate)))
        return eff_rate * retrieved + (1.0 - eff_rate) * npy

    def _decode_single_slot(
        self,
        slot: RvcSlotModel,
        feats_100fps_t: torch.Tensor,
        pitch_t: torch.Tensor,
        pitchf_t: torch.Tensor,
        p_len_t: torch.Tensor,
        sid_t: torch.Tensor,
        shared_eps: torch.Tensor = None,
    ):
        """
        Runs enc_p -> flow -> dec for a loaded RVC slot, sharing the deterministic
        prior noise tensor `shared_eps` so dual-slot morphing is strictly phase-coherent.
        """
        net_g = slot.net_g
        with torch.no_grad():
            if slot.if_f0 == 1:
                g = net_g.emb_g(sid_t).unsqueeze(-1)
                m_p, logs_p, x_mask = net_g.enc_p(feats_100fps_t, pitch_t, p_len_t)
                if shared_eps is None or shared_eps.shape != m_p.shape:
                    eps = torch.randn_like(m_p) * 0.66666
                else:
                    eps = shared_eps.to(dtype=m_p.dtype, device=m_p.device)
                z_p = (m_p + torch.exp(logs_p) * eps) * x_mask
                z = net_g.flow(z_p, x_mask, g=g, reverse=True)
                o = net_g.dec(z * x_mask, pitchf_t, g=g, n_res=None)
                wav = o[0, 0].float().cpu().numpy()
                return wav, z, g, x_mask, eps
            else:
                o, _, _ = net_g.infer(feats_100fps_t, p_len_t, sid_t)
                wav = o[0, 0].float().cpu().numpy()
                return wav, None, None, None, None

    def _apply_digital_silence_gate(
        self,
        dry_16k: np.ndarray,
        wet_wav: np.ndarray,
        target_sr: int,
        uv_unpadded: np.ndarray = None,
    ) -> np.ndarray:
        """
        Smooth soft-knee gate that:
        1) Attenuates VITS generator idle noise during true digital silence (< -84 dB, dry RMS < 6e-5),
        2) Clamps VITS envelope inflation during sustained unvoiced breath segments (f0 == 0) so quiet
           breaths are never over-amplified into audible gasps/moans, while keeping gain strictly 1.000
           across all voiced singing (f0 > 0) and consonant onset/release boundaries.
        """
        if len(wet_wav) < 256 or len(dry_16k) < 160:
            return wet_wav

        n_frames = max(4, len(dry_16k) // 160)
        dry_pad = np.pad(dry_16k ** 2, (320, 320), mode="reflect")
        dry_kernel = np.ones(640, dtype=np.float32) / 640.0
        dry_ms = np.convolve(dry_pad, dry_kernel, mode="valid")[::160][:n_frames]
        dry_rms_frames = np.sqrt(np.maximum(dry_ms, 1e-12))

        # Soft-knee transition only below 6e-5 (-84 dB); strictly 1.0 everywhere >= 6e-5
        gate_frames = np.clip((dry_rms_frames - 1e-5) / 5e-5, 0.0, 1.0).astype(np.float32)
        gate_frames = gate_frames * gate_frames * (3.0 - 2.0 * gate_frames)

        # In sustained unvoiced/breath regions (f0 == 0, >20ms away from any voiced vowel),
        # prevent VITS latent normalization from inflating quiet breath RMS above dry breath RMS.
        if uv_unpadded is not None and len(uv_unpadded) >= 4:
            uv_f = np.resize(uv_unpadded.astype(bool), n_frames)
            voiced_f = ~uv_f
            # Dilate voiced mask by 2 frames (20ms) on each side to protect consonant attacks/releases
            voiced_dilated = np.convolve(
                voiced_f.astype(np.int32), np.ones(5, dtype=np.int32), mode="same"
            ) > 0
            breath_mask = ~voiced_dilated
            if np.any(breath_mask):
                hop_wet = max(1, int(round(target_sr * 0.01)))
                win_wet = max(4, hop_wet * 4)
                half_w = win_wet // 2
                wet_pad = np.pad(wet_wav.astype(np.float32) ** 2, (half_w, half_w), mode="reflect")
                wet_cs = np.concatenate(([0.0], np.cumsum(wet_pad, dtype=np.float64)))
                f_idx = np.arange(n_frames) * hop_wet
                f_end = np.minimum(f_idx + win_wet, len(wet_pad))
                wet_ms = (wet_cs[f_end] - wet_cs[f_idx]) / np.maximum(f_end - f_idx, 1)
                wet_rms_frames = np.sqrt(np.maximum(wet_ms.astype(np.float32), 1e-12))

                breath_cap = np.ones(n_frames, dtype=np.float32)
                cap_vals = np.clip(
                    (dry_rms_frames[breath_mask] * 1.05 + 1e-5) / np.maximum(wet_rms_frames[breath_mask], 1e-6),
                    0.05,
                    1.0,
                )
                breath_cap[breath_mask] = cap_vals
                gate_frames = gate_frames * breath_cap

        # Smooth gate across 5 frames (50ms) so transitions into/out of breaths and silence are click-free
        if len(gate_frames) >= 5:
            smooth_k = np.array([0.1, 0.2, 0.4, 0.2, 0.1], dtype=np.float32)
            gate_pad = np.pad(gate_frames, (2, 2), mode="edge")
            gate_frames = np.convolve(gate_pad, smooth_k, mode="valid").astype(np.float32)

        sample_x = np.linspace(0.0, float(len(gate_frames) - 1), len(wet_wav), dtype=np.float32)
        frame_x = np.arange(len(gate_frames), dtype=np.float32)
        gain_samples = np.interp(sample_x, frame_x, gate_frames).astype(np.float32)

        out = wet_wav * gain_samples
        peak = float(np.max(np.abs(out))) if len(out) > 0 else 0.0
        if peak > 0.98:
            out *= 0.98 / peak
        return out.astype(np.float32)

    def synthesize_chunk(
        self,
        audio_16k: np.ndarray,
        ab_morph: float,
        inject_depth: float,
        pitch_shift_st: float,
        f0_filter_radius: float,
        index_rate: float,
        protect: float,
        slot_a_index_rate: float,
        slot_b_index_rate: float,
        rms_mix_rate: float,
        target_sr: int,
        auto_curves: np.ndarray = None,
        progress_base: float = 0.0,
        progress_span: float = 1.0,
    ):
        t_start = time.perf_counter()
        if audio_16k.ndim != 1 or not len(audio_16k) or not np.isfinite(audio_16k).all():
            raise ValueError("Expected non-empty finite mono audio")
        if not np.isfinite([ab_morph, inject_depth, pitch_shift_st, f0_filter_radius, index_rate,
                            protect, slot_a_index_rate, slot_b_index_rate, rms_mix_rate]).all():
            raise ValueError("Expected finite synthesis parameters")
        if not 8000 <= target_sr <= 384000:
            raise ValueError(f"Unsupported output sample rate {target_sr}")
        if auto_curves is not None and (auto_curves.ndim != 2 or auto_curves.shape[1] != 4 or not np.isfinite(auto_curves).all()):
            raise ValueError("Expected finite four-column automation curves")

        slot_a = self.slots[0]
        slot_b = self.slots[1]

        # If neither slot has a loaded .pth model, return resampled input audio for both slots
        if not slot_a.has_pth and not slot_b.has_pth:
            out_len = int(round(len(audio_16k) * float(target_sr) / 16000.0))
            out_wav = signal.resample_poly(audio_16k, target_sr // math.gcd(target_sr, 16000), 16000 // math.gcd(target_sr, 16000))
            if len(out_wav) > out_len:
                out_wav = out_wav[:out_len]
            elif len(out_wav) < out_len:
                out_wav = np.pad(out_wav, (0, out_len - len(out_wav)))
            out_wav = out_wav.astype(np.float32)
            env = compute_64bin_scope_envelope(audio_16k, 16000)
            return out_wav, out_wav.copy(), 0.0, 1.0, env, env, env, env

        # 1. Peak-clamp (>0.95) and pre-filter 16kHz audio with 48Hz 5th-order Butterworth high-pass filter
        # (per D:\RVC20260718Nvidia\infer\vc\modules.py:181-183 & pipeline.py:20,56,286,306,332),
        # and pad with t_pad = 3200 (0.2s left) and t_pad + window = 3360 (0.21s right)
        # so HuBERT's 400-sample conv receptive field aligns 1-to-1 with p_len without frame truncation.
        audio_in = audio_16k.astype(np.float32)
        audio_max = float(np.max(np.abs(audio_in))) / 0.95 if len(audio_in) > 0 else 0.0
        if audio_max > 1.0:
            audio_in = (audio_in / audio_max).astype(np.float32)
        if len(audio_in) > 32:
            filtered_16k = signal.filtfilt(HIGHPASS_BH, HIGHPASS_AH, audio_in).astype(np.float32)
        else:
            filtered_16k = audio_in
        pad_16k = 3200
        pad_16k_right = 3200 + 160
        padded_16k = np.pad(filtered_16k, (pad_16k, pad_16k_right), mode="reflect").astype(np.float32)

        # 2. Extract or fetch cached HuBERT features & RMVPE base F0
        (feats_768_np, feats_256_np, f0_base_hz, dry_env_64, retrieved_cache), _ = self._extract_or_get_cached_features(
            padded_16k, progress_base, progress_span
        )

        # Compute median voiced F0 for UI telemetry
        voiced_f0 = f0_base_hz[f0_base_hz > 55.0]
        detected_f0_hz = float(np.median(voiced_f0)) if len(voiced_f0) > 0 else 0.0

        # 3. Determine active slots and scalar/curve blend parameters
        alpha = float(max(0.0, min(1.0, ab_morph)))
        if not slot_a.has_pth:
            alpha = 1.0
        elif not slot_b.has_pth:
            alpha = 0.0

        n_50fps = feats_768_np.shape[0]
        morph_curve_50fps = None
        slot_a_idx_50fps = None
        slot_b_idx_50fps = None

        if auto_curves is not None and auto_curves.ndim == 2 and auto_curves.shape[0] >= 1 and auto_curves.shape[1] >= 4:
            # auto_curves columns: [0]=ab_morph, [1]=inject_depth, [2]=slot_a_index_rate, [3]=slot_b_index_rate
            morph_curve_50fps = np.clip(sample_automation_curve(auto_curves, 0, n_50fps, 320, pad_16k), 0.0, 1.0)
            if not slot_a.has_pth:
                morph_curve_50fps.fill(1.0)
            elif not slot_b.has_pth:
                morph_curve_50fps.fill(0.0)
            alpha = float(np.mean(morph_curve_50fps))

            slot_a_idx_50fps = np.clip(sample_automation_curve(auto_curves, 2, n_50fps, 320, pad_16k), 0.0, 1.0)
            slot_b_idx_50fps = np.clip(sample_automation_curve(auto_curves, 3, n_50fps, 320, pad_16k), 0.0, 1.0)

        send_progress(
            progress_base + progress_span * 0.48,
            f"RETRIEVING FAISS TIMBRE INDICES (A:{int((1.0 - alpha) * 100)}% / B:{int(alpha * 100)}%)...",
        )

        # Effective global index retrieval rate
        g_index_rate = float(max(0.0, min(1.0, index_rate)))

        # Prepare 768D (v2) and/or 256D (v1) retrieved features for Slot A & Slot B
        base_a_np = feats_256_np if slot_a.feature_dim == 256 else feats_768_np
        base_b_np = feats_256_np if slot_b.feature_dim == 256 else feats_768_np
        feats_a = self._retrieve_slot_features(
            slot_a, base_a_np, g_index_rate, slot_a_index_rate, retrieved_cache, slot_a_idx_50fps
        )
        feats_b = self._retrieve_slot_features(
            slot_b, base_b_np, g_index_rate, slot_b_index_rate, retrieved_cache, slot_b_idx_50fps
        )
        if feats_a.shape == feats_b.shape:
            if morph_curve_50fps is not None and len(morph_curve_50fps) == feats_a.shape[0]:
                mc = morph_curve_50fps[:, None]
                feats_mix_768 = (1.0 - mc) * feats_a + mc * feats_b
            else:
                feats_mix_768 = (1.0 - alpha) * feats_a + alpha * feats_b
            raw_base_np = base_a_np
        else:
            feats_mix_768 = feats_a if alpha < 0.5 else feats_b
            raw_base_np = base_a_np if alpha < 0.5 else base_b_np

        # Upsample Slot A & Slot B features 2x along time (from 50 fps to 100 fps) independently
        # per D:\RVC20260718Nvidia\infer\vc\pipeline.py:198-206
        p_len = min(len(f0_base_hz), int((len(audio_16k) + 2 * pad_16k) // 160))

        def prepare_100fps_tensor(feats_np: np.ndarray, raw_feats_np: np.ndarray):
            f_t = torch.from_numpy(feats_np).unsqueeze(0).to(self.device)
            f0_t = torch.from_numpy(raw_feats_np).unsqueeze(0).to(self.device)
            if self.is_half:
                f_t = f_t.half()
                f0_t = f0_t.half()
            else:
                f_t = f_t.float()
                f0_t = f0_t.float()

            f_t = F.interpolate(f_t.permute(0, 2, 1), scale_factor=2.0, mode="nearest").permute(0, 2, 1)
            f0_t = F.interpolate(f0_t.permute(0, 2, 1), scale_factor=2.0, mode="nearest").permute(0, 2, 1)
            cur_len = min(p_len, f_t.shape[1], f0_t.shape[1])
            f_t = f_t[:, :cur_len, :]
            f0_t = f0_t[:, :cur_len, :]
            return f_t, f0_t, cur_len

        feats_a_t, feats0_a_t, cur_len_a = prepare_100fps_tensor(feats_a, base_a_np)
        feats_b_t, feats0_b_t, cur_len_b = prepare_100fps_tensor(feats_b, base_b_np)
        cur_len = min(cur_len_a, cur_len_b)
        feats_a_t = feats_a_t[:, :cur_len, :]
        feats0_a_t = feats0_a_t[:, :cur_len, :]
        feats_b_t = feats_b_t[:, :cur_len, :]
        feats0_b_t = feats0_b_t[:, :cur_len, :]

        # 4. Authentic RVC v2 Per-Slot F0 Unvoiced Hard Gate (f0 == 0 -> pitch=1, pitchf=0.0),
        #    Pitch Shift (f0_up_key), and Unvoiced Breath / Consonant Feature Protection.
        #    Do NOT linearly interpolate f0 across unvoiced/breath segments: SineGen._f02uv (models.py:300-362)
        #    requires f0 == 0 on unvoiced frames so the harmonic sine generator is shut off (uv = 0) and
        #    only unvoiced aspiration noise is excited.
        f0_raw_cur = f0_base_hz[:cur_len].copy()
        uv_raw_mask = f0_raw_cur <= 0.0

        slot_a_up_key = float(max(-24.0, min(24.0, pitch_shift_st)))
        slot_b_up_key = float(max(-24.0, min(24.0, f0_filter_radius)))

        def build_slot_pitch_tensors(f0_base: np.ndarray, up_key_st: float):
            f0_s = np.zeros_like(f0_base, dtype=np.float32)
            voiced_mask = f0_base > 0.0
            f0_s[voiced_mask] = f0_base[voiced_mask] * float(2.0 ** (up_key_st / 12.0))
            f0bak = f0_s.copy()

            f0_mel = np.zeros_like(f0_s, dtype=np.float32)
            f0_mel[voiced_mask] = 1127.0 * np.log(1.0 + f0_s[voiced_mask] / 700.0)
            v_mask = f0_mel > 0.0
            f0_mel[v_mask] = (f0_mel[v_mask] - self.f0_mel_min) * 254.0 / (
                self.f0_mel_max - self.f0_mel_min
            ) + 1.0
            f0_mel[f0_mel <= 1.0] = 1.0
            f0_mel[f0_mel > 255.0] = 255.0
            f0_coarse = np.rint(f0_mel).astype(np.int64)

            p_t = torch.from_numpy(f0_coarse).unsqueeze(0).to(self.device).long()
            pf_t = torch.from_numpy(f0bak).unsqueeze(0).to(self.device)
            if self.is_half:
                pf_t = pf_t.half()
            else:
                pf_t = pf_t.float()
            return p_t, pf_t

        pitch_a_t, pitchf_a_t = build_slot_pitch_tensors(f0_raw_cur, slot_a_up_key)
        pitch_b_t, pitchf_b_t = build_slot_pitch_tensors(f0_raw_cur, slot_b_up_key)

        # Reference RVC protect: 0 keeps raw unvoiced features, 0.5 disables protection.
        eff_protect = float(max(0.0, min(0.5, protect)))
        if eff_protect < 0.5:
            ff_np = np.where(uv_raw_mask, eff_protect, 1.0).astype(np.float32)
            ff_a = torch.from_numpy(ff_np).to(device=self.device, dtype=feats_a_t.dtype).view(1, cur_len, 1)
            ff_b = torch.from_numpy(ff_np).to(device=self.device, dtype=feats_b_t.dtype).view(1, cur_len, 1)
            if slot_a.if_f0 == 1:
                feats_a_t = (feats_a_t * ff_a + feats0_a_t * (1.0 - ff_a)).to(feats0_a_t.dtype)
            if slot_b.if_f0 == 1:
                feats_b_t = (feats_b_t * ff_b + feats0_b_t * (1.0 - ff_b)).to(feats0_b_t.dtype)

        p_len_t = torch.tensor([cur_len], device=self.device, dtype=torch.long)
        sid_t = torch.tensor([0], device=self.device, dtype=torch.long)

        # 5. Dual-Slot Phase-Locked VITS + NSF-HiFiGAN Basis Waveform Synthesis
        send_progress(
            progress_base + progress_span * 0.72,
            "RUNNING DUAL-SLOT VITS + NSF-HIFIGAN BASIS GENERATOR...",
        )

        expected_len = int(round(len(audio_16k) * float(target_sr) / 16000.0))
        pad_100fps = pad_16k // 160
        unpadded_100fps = max(1, len(audio_16k) // 160)
        uv_unpadded = uv_raw_mask[pad_100fps : pad_100fps + unpadded_100fps]

        def trim_and_resample_basis(wav_in: np.ndarray, src_sr: int, dst_sr: int) -> np.ndarray:
            # Trim t_pad_tgt in the model's native sample rate FIRST (per pipeline.py:57, 341, 398-401)
            # where each 100fps frame is an exact integer number of samples (src_sr // 100),
            # then resample the unpadded audio to dst_sr for 0-sample timing alignment.
            pad_src = int(round(pad_16k * float(src_sr) / 16000.0))
            expected_len_src = int(round(len(audio_16k) * float(src_sr) / 16000.0))
            w = wav_in
            if len(w) > pad_src:
                w = w[pad_src : pad_src + expected_len_src]
            if len(w) > expected_len_src:
                w = w[:expected_len_src]
            elif len(w) < expected_len_src and len(w) > 0:
                w = np.pad(w, (0, expected_len_src - len(w)), mode="edge")
            elif len(w) == 0:
                w = np.zeros(expected_len_src, dtype=np.float32)

            if src_sr != dst_sr:
                g_div = math.gcd(dst_sr, src_sr)
                w = signal.resample_poly(w, dst_sr // g_div, src_sr // g_div).astype(np.float32)

            if len(w) > expected_len:
                w = w[:expected_len]
            elif len(w) < expected_len and len(w) > 0:
                w = np.pad(w, (0, expected_len - len(w)), mode="edge")
            elif len(w) == 0:
                w = np.zeros(expected_len, dtype=np.float32)

            w = self._apply_digital_silence_gate(audio_16k, w, dst_sr, uv_unpadded=uv_unpadded).astype(np.float32)
            peak = float(np.max(np.abs(w))) if len(w) > 0 else 0.0
            if peak > 0.98:
                w = (w * (0.98 / peak)).astype(np.float32)
            return w

        torch.manual_seed(42)
        if slot_a.has_pth and slot_b.has_pth:
            wav_a_raw, _, _, _, shared_eps = self._decode_single_slot(
                slot_a, feats_a_t, pitch_a_t, pitchf_a_t, p_len_t, sid_t, shared_eps=None
            )
            torch.manual_seed(42)
            wav_b_raw, _, _, _, _ = self._decode_single_slot(
                slot_b, feats_b_t, pitch_b_t, pitchf_b_t, p_len_t, sid_t, shared_eps=shared_eps
            )
            wav_a_out = trim_and_resample_basis(wav_a_raw, slot_a.sr, target_sr)
            wav_b_out = trim_and_resample_basis(wav_b_raw, slot_b.sr, target_sr)
        elif slot_a.has_pth:
            wav_a_raw, _, _, _, _ = self._decode_single_slot(
                slot_a, feats_a_t, pitch_a_t, pitchf_a_t, p_len_t, sid_t, shared_eps=None
            )
            wav_a_out = trim_and_resample_basis(wav_a_raw, slot_a.sr, target_sr)
            wav_b_out = wav_a_out.copy()
        else:
            wav_b_raw, _, _, _, _ = self._decode_single_slot(
                slot_b, feats_b_t, pitch_b_t, pitchf_b_t, p_len_t, sid_t, shared_eps=None
            )
            wav_b_out = trim_and_resample_basis(wav_b_raw, slot_b.sr, target_sr)
            wav_a_out = wav_b_out.copy()

        # 6. Compute 64-bin Scope Envelopes for UI Display
        slot_a_env_64 = compute_64bin_scope_envelope(wav_a_out, target_sr)
        slot_b_env_64 = compute_64bin_scope_envelope(wav_b_out, target_sr)
        mix_preview = ((1.0 - alpha) * wav_a_out + alpha * wav_b_out).astype(np.float32)
        out_env_64 = compute_64bin_scope_envelope(mix_preview, target_sr)

        elapsed_ms = float((time.perf_counter() - t_start) * 1000.0)
        send_progress(progress_base + progress_span, f"RVC DUAL-BASIS CACHE READY ({elapsed_ms:.0f} MS)")
        return (
            wav_a_out.astype(np.float32),
            wav_b_out.astype(np.float32),
            detected_f0_hz,
            elapsed_ms,
            dry_env_64.astype(np.float32),
            slot_a_env_64.astype(np.float32),
            slot_b_env_64.astype(np.float32),
            out_env_64.astype(np.float32),
        )


# ---------------------------------------------------------------------------
# 5. Main Binary Pipe Event Loop
# ---------------------------------------------------------------------------
def run_daemon_loop():
    engine = RvcDaemonEngine()
    while True:
        hdr = read_exact(IPC_IN, 12)
        if not hdr or len(hdr) < 12:
            break

        magic, cmd, payload_len = struct.unpack("<4sII", hdr)
        if magic != REQ_MAGIC or payload_len > 128 * 1024 * 1024:
            break

        payload = read_exact(IPC_IN, payload_len) if payload_len > 0 else b""
        if payload_len > 0 and len(payload) < payload_len:
            break

        try:
            if cmd == CMD_SHUTDOWN:
                break

            elif cmd == CMD_INIT:
                engine.ensure_base_models()
                # Do NOT pre-load any default singer models on startup; slots remain UNLOADED
                # until explicitly imported by the user (or loaded by offline test harness).
                s0 = engine.slots[0]
                s1 = engine.slots[1]
                send_packet(MSG_STATUS_OK, engine.pack_slot_info(s0) + engine.pack_slot_info(s1))

            elif cmd == CMD_LOAD_SLOT:
                slot_idx, path_str, index_path = unpack_load_slot_payload(payload)
                slot = engine.load_slot(slot_idx, path_str, index_path)
                send_packet(MSG_SLOT_INFO, engine.pack_slot_info(slot))

            elif cmd == CMD_LOAD_PRESET:
                slot_idx, preset_idx = struct.unpack("<II", payload[:8])
                slot = engine.load_preset(slot_idx, preset_idx)
                send_packet(MSG_SLOT_INFO, engine.pack_slot_info(slot))

            elif cmd == CMD_CLEAR_SLOT:
                (slot_idx,) = struct.unpack("<I", payload[:4])
                slot = engine.clear_slot(slot_idx)
                send_packet(MSG_SLOT_INFO, engine.pack_slot_info(slot))

            elif cmd == CMD_SYNTH_CHUNK:
                # Unpack synthesis parameters + float32 16kHz input samples + optional 10ms automation curves
                # Extended Header: 9 floats + 3 uint32s = 48 bytes
                if len(payload) < 48:
                    raise ValueError("Truncated synthesis header")
                (
                    ab_morph,
                    inject_depth,
                    pitch_shift_st,
                    f0_filter_radius,
                    index_rate,
                    protect,
                    slot_a_index_rate,
                    slot_b_index_rate,
                    rms_mix_rate,
                    target_sr,
                    num_samples_16k,
                    num_auto_frames,
                ) = struct.unpack("<fffffffffIII", payload[:48])

                audio_end = 48 + num_samples_16k * 4
                curves_end = audio_end + num_auto_frames * 4 * 4
                if curves_end != len(payload):
                    raise ValueError("Invalid synthesis audio/automation payload length")
                audio_bytes = payload[48:audio_end]
                audio_16k = np.frombuffer(audio_bytes, dtype=np.float32).copy()

                auto_curves = None
                if num_auto_frames > 0:
                    raw_curves = np.frombuffer(payload[audio_end:curves_end], dtype=np.float32).copy()
                    auto_curves = raw_curves.reshape((num_auto_frames, 4))

                (
                    wav_a_out,
                    wav_b_out,
                    f0_hz,
                    elapsed_ms,
                    dry_env,
                    slot_a_env,
                    slot_b_env,
                    out_env,
                ) = engine.synthesize_chunk(
                    audio_16k=audio_16k,
                    ab_morph=ab_morph,
                    inject_depth=inject_depth,
                    pitch_shift_st=pitch_shift_st,
                    f0_filter_radius=f0_filter_radius,
                    index_rate=index_rate,
                    protect=protect,
                    slot_a_index_rate=slot_a_index_rate,
                    slot_b_index_rate=slot_b_index_rate,
                    rms_mix_rate=rms_mix_rate,
                    target_sr=int(target_sr),
                    auto_curves=auto_curves,
                )

                res_hdr = struct.pack("<ffI", float(f0_hz), float(elapsed_ms), len(wav_a_out))
                envs_bytes = (
                    dry_env.tobytes()
                    + slot_a_env.tobytes()
                    + slot_b_env.tobytes()
                    + out_env.tobytes()
                )
                send_packet(MSG_SYNTH_RESULT, res_hdr + envs_bytes + wav_a_out.tobytes() + wav_b_out.tobytes())

            else:
                send_error(f"Unknown command ID: {cmd}")

        except Exception as e:
            tb = traceback.format_exc()
            print(f"[DAEMON EXCEPTION] {e}\n{tb}", file=sys.stderr)
            send_error(str(e))


def run_cli_self_test():
    print("=== Running RVC Daemon Worker CLI Self-Test ===", file=sys.stderr)
    engine = RvcDaemonEngine()
    engine.ensure_base_models()
    s0 = engine.load_slot(0, _runtime_args.model_a, _runtime_args.index_a)
    s1 = engine.load_slot(1, _runtime_args.model_b, _runtime_args.index_b)
    print(f"Slot A: {s0.model_name} | {s0.arch_tag} | vectors={s0.vector_count}", file=sys.stderr)
    print(f"Slot B: {s1.model_name} | {s1.arch_tag} | vectors={s1.vector_count}", file=sys.stderr)

    # Generate 2.0 seconds of synthetic sung vocal at 16kHz (F0 = 220 Hz with vibrato + formants)
    sr = 16000
    t = np.arange(sr * 2, dtype=np.float32) / float(sr)
    f0 = 220.0 + 4.0 * np.sin(2.0 * np.pi * 5.2 * t)
    phase = np.cumsum(f0 / float(sr))
    vocal = np.zeros_like(t)
    for h in range(1, 28):
        hf = h * 220.0
        env = (
            np.exp(-0.5 * ((hf - 700.0) / 250.0) ** 2)
            + 0.7 * np.exp(-0.5 * ((hf - 1800.0) / 380.0) ** 2)
            + 0.4 * np.exp(-0.5 * ((hf - 3100.0) / 500.0) ** 2)
        )
        vocal += (0.08 * env / (h ** 0.6)) * np.sin(2.0 * np.pi * h * phase)
    vocal = vocal.astype(np.float32)

    # 1st synthesis (cold cache: runs HuBERT + RMVPE + FAISS + Dual VITS)
    wav1_a, wav1_b, f0_det, ms1, _, _, _, _ = engine.synthesize_chunk(
        vocal,
        ab_morph=0.35,
        inject_depth=1.00,
        pitch_shift_st=0.0,
        f0_filter_radius=0.0,
        index_rate=1.00,
        protect=0.33,
        slot_a_index_rate=0.75,
        slot_b_index_rate=0.75,
        rms_mix_rate=0.25,
        target_sr=48000,
    )
    print(
        f"Cold Synth (2.0s audio -> {len(wav1_a)} samples @ 48kHz): {ms1:.1f} ms, F0={f0_det:.1f} Hz, RMS_A={np.sqrt(np.mean(wav1_a**2)):.4f}, RMS_B={np.sqrt(np.mean(wav1_b**2)):.4f}",
        file=sys.stderr,
    )

    # 2nd synthesis (warm feature cache + 0%->100% Index Rate automation curve!)
    n_auto = 200
    ramp = np.linspace(0.0, 1.0, n_auto, dtype=np.float32)
    auto_curves = np.stack([ramp, np.ones_like(ramp), ramp, 1.0 - ramp], axis=1)
    wav2_a, wav2_b, f0_det2, ms2, _, _, _, _ = engine.synthesize_chunk(
        vocal,
        ab_morph=0.50,
        inject_depth=1.00,
        pitch_shift_st=12.0,
        f0_filter_radius=-12.0,
        index_rate=1.00,
        protect=0.33,
        slot_a_index_rate=0.80,
        slot_b_index_rate=0.80,
        rms_mix_rate=0.25,
        target_sr=48000,
        auto_curves=auto_curves,
    )
    print(
        f"Warm Cache Re-Synth (with 10ms automation curves): {ms2:.1f} ms, F0={f0_det2:.1f} Hz, RMS_A={np.sqrt(np.mean(wav2_a**2)):.4f}, RMS_B={np.sqrt(np.mean(wav2_b**2)):.4f}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    if IS_TEST_CLI:
        if not _runtime_args.model_a or not _runtime_args.model_b:
            _args.error("--test-cli requires explicit --model-a and --model-b checkpoint paths")
        run_cli_self_test()
    else:
        run_daemon_loop()
