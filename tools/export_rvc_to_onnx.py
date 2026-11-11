#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Legacy synthetic spectral-profile demo generator, not a RVC network exporter.

The current VST3 loads genuine .pth checkpoints and FAISS .index files directly.
--generate-presets only creates artificial demo profiles for old experiments.
"""

import argparse
import os
import struct
import sys
from pathlib import Path
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper
import onnxruntime as ort


RVCFEAT_MAGIC = b"RVCF"
RVCFEAT_VERSION = 1
FEATURE_DIM = 128
NUM_CENTROIDS = 64


def slaney_mel_frequencies(n_mels: int = 128, fmin: float = 0.0, fmax: float = 16000.0) -> np.ndarray:
    """Computes center frequencies of Slaney-scale Mel bins."""
    f_sp = 200.0 / 3.0
    min_log_hz = 1000.0
    min_log_mel = (min_log_hz - fmin) / f_sp
    logstep = np.log(6.4) / 27.0

    def hz_to_mel(hz):
        if hz < min_log_hz:
            return (hz - fmin) / f_sp
        return min_log_mel + np.log(hz / min_log_hz) / logstep

    def mel_to_hz(mel):
        if mel < min_log_mel:
            return fmin + f_sp * mel
        return min_log_hz * np.exp(logstep * (mel - min_log_mel))

    mel_min = hz_to_mel(fmin)
    mel_max = hz_to_mel(fmax)
    mels = np.linspace(mel_min, mel_max, n_mels)
    return np.array([mel_to_hz(m) for m in mels], dtype=np.float32)


def design_singer_profile(
    profile_name: str,
    formants_hz: list[tuple[float, float, float]],
    harmonic_tilt_db_per_oct: float,
    air_boost_db: float,
    air_freq_hz: float,
    seed: int,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """
    Constructs a neural timbre projection matrix W_proj [128, 128],
    target formant bias b_formant [128], speaker embedding g [128],
    and K-Means feature centroids [64, 128].
    """
    rng = np.random.default_rng(seed)
    freqs = slaney_mel_frequencies(FEATURE_DIM, 40.0, 16000.0)

    # 1. Target vocal tract formant envelope in dB (normalized around 0 dB)
    formant_curve_db = np.zeros(FEATURE_DIM, dtype=np.float32)
    for f_center, gain_db, bw_hz in formants_hz:
        sigma = max(bw_hz, 40.0)
        # Asymmetric log-frequency Gaussian resonance peak
        log_dist = np.log2(np.maximum(freqs, 40.0) / max(f_center, 40.0))
        sigma_oct = np.log2(1.0 + sigma / max(f_center, 40.0))
        formant_curve_db += gain_db * np.exp(-0.5 * (log_dist / max(sigma_oct, 0.08)) ** 2)

    # Harmonic spectral tilt relative to 1 kHz
    octaves_from_1k = np.log2(np.maximum(freqs, 60.0) / 1000.0)
    formant_curve_db += harmonic_tilt_db_per_oct * octaves_from_1k

    # High-frequency breath/air shelf
    air_Mask = 1.0 / (1.0 + np.exp(-(freqs - air_freq_hz) / 800.0))
    formant_curve_db += air_boost_db * air_Mask.astype(np.float32)

    # Remove DC offset so average energy stays neutral
    formant_curve_db -= np.mean(formant_curve_db[:96])

    # 2. Cross-band neural formant projection matrix W_proj [128, 128]
    # Combines local spectral smoothing + cross-harmonic coupling (f0 -> 2f0, 3f0 resonances)
    W_proj = np.eye(FEATURE_DIM, dtype=np.float32) * 0.68
    for i in range(FEATURE_DIM):
        for j in range(FEATURE_DIM):
            dist = abs(i - j)
            if 1 <= dist <= 4:
                W_proj[i, j] += 0.12 * np.exp(-0.5 * (dist / 1.8) ** 2)
        # Subtle harmonic coupling from fundamental/low-mid bands into singer's formant region
        if 40 <= i <= 95:
            src_band = max(8, i // 2)
            W_proj[i, src_band] += 0.065

    # Normalize rows of W_proj
    row_sums = np.sum(W_proj, axis=1, keepdims=True)
    W_proj = (W_proj / np.maximum(row_sums, 1e-6)).astype(np.float32)

    # 3. Speaker embedding vector g [128]
    speaker_emb = formant_curve_db / 12.0 + rng.normal(0.0, 0.08, size=FEATURE_DIM).astype(np.float32)
    speaker_emb = speaker_emb.astype(np.float32)

    # 4. Feature Retrieval Centroids [NUM_CENTROIDS, 128] representing vowel/phoneme articulations
    centroids = np.zeros((NUM_CENTROIDS, FEATURE_DIM), dtype=np.float32)
    vowel_shifts = np.linspace(-0.22, 0.25, NUM_CENTROIDS, dtype=np.float32)
    for k in range(NUM_CENTROIDS):
        shift_bins = int(round(vowel_shifts[k] * 14))
        shifted_env = np.roll(formant_curve_db, shift_bins)
        if shift_bins > 0:
            shifted_env[:shift_bins] = formant_curve_db[0]
        elif shift_bins < 0:
            shifted_env[shift_bins:] = formant_curve_db[-1]
        variation = rng.normal(0.0, 0.65, size=FEATURE_DIM).astype(np.float32)
        # Smooth variation across bins
        variation = np.convolve(variation, [0.2, 0.6, 0.2], mode="same").astype(np.float32)
        centroids[k] = shifted_env + variation

    return W_proj, formant_curve_db, speaker_emb, centroids


def build_rvc_timbre_onnx_model(
    model_name: str,
    W_proj: np.ndarray,
    formant_bias_db: np.ndarray,
    speaker_emb: np.ndarray,
    centroids: np.ndarray,
) -> onnx.ModelProto:
    """
    Creates an artificial ONNX spectral-profile graph for legacy experiments:
    Inputs:
      - mel: float32 [1, 128, T] (Incoming vocal Log-Mel / Cepstral envelope frame)
    Outputs:
      - timbre_env: float32 [1, 128, T] (Projected RVC target timbre envelope in dB)
      - speaker_emb: float32 [1, 128] (Target singer latent embedding vector)
    """
    mel_in = helper.make_tensor_value_info("mel", TensorProto.FLOAT, [1, FEATURE_DIM, "T"])
    timbre_out = helper.make_tensor_value_info("timbre_env", TensorProto.FLOAT, [1, FEATURE_DIM, "T"])
    spk_out = helper.make_tensor_value_info("speaker_emb", TensorProto.FLOAT, [1, FEATURE_DIM])

    # Hidden non-linear residual projection weights (128 -> 128 MLP + Skip connection)
    rng = np.random.default_rng(abs(hash(model_name)) % (2**31))
    W_res1 = (rng.normal(0.0, 0.04, size=(FEATURE_DIM, FEATURE_DIM)) + np.eye(FEATURE_DIM) * 0.15).astype(np.float32)
    W_res2 = (rng.normal(0.0, 0.04, size=(FEATURE_DIM, FEATURE_DIM)) + np.eye(FEATURE_DIM) * 0.15).astype(np.float32)

    init_W_proj = numpy_helper.from_array(W_proj.reshape(1, FEATURE_DIM, FEATURE_DIM), name="W_proj")
    init_W_res1 = numpy_helper.from_array(W_res1.reshape(1, FEATURE_DIM, FEATURE_DIM), name="W_res1")
    init_W_res2 = numpy_helper.from_array(W_res2.reshape(1, FEATURE_DIM, FEATURE_DIM), name="W_res2")
    init_bias = numpy_helper.from_array(formant_bias_db.reshape(1, FEATURE_DIM, 1), name="formant_bias")
    init_spk = numpy_helper.from_array(speaker_emb.reshape(1, FEATURE_DIM), name="spk_const")

    nodes = [
        # Linear formant transfer: base_proj = MatMul(W_proj, mel)
        helper.make_node("MatMul", inputs=["W_proj", "mel"], outputs=["base_proj"], name="MatMul_BaseProj"),
        # Non-linear harmonic residual branch: res1 = Tanh(MatMul(W_res1, mel))
        helper.make_node("MatMul", inputs=["W_res1", "mel"], outputs=["res1_pre"], name="MatMul_Res1"),
        helper.make_node("Tanh", inputs=["res1_pre"], outputs=["res1_act"], name="Tanh_Res1"),
        helper.make_node("MatMul", inputs=["W_res2", "res1_act"], outputs=["res2_out"], name="MatMul_Res2"),
        # Combine base projection + residual + target singer formant bias
        helper.make_node("Add", inputs=["base_proj", "res2_out"], outputs=["combined_proj"], name="Add_Residual"),
        helper.make_node("Add", inputs=["combined_proj", "formant_bias"], outputs=["timbre_env"], name="Add_FormantBias"),
        # Identity output for speaker embedding metadata
        helper.make_node("Identity", inputs=["spk_const"], outputs=["speaker_emb"], name="Identity_SpeakerEmb"),
    ]

    graph = helper.make_graph(
        nodes=nodes,
        name=f"RVC_TimbreInjector_{model_name}",
        inputs=[mel_in],
        outputs=[timbre_out, spk_out],
        initializer=[init_W_proj, init_W_res1, init_W_res2, init_bias, init_spk],
    )

    model = helper.make_model(
        graph,
        producer_name="RVC_FeatureInjector_VST3",
        opset_imports=[helper.make_opsetid("", 14)],
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def write_rvcfeat_file(
    output_path: Path,
    model_name: str,
    formant_bias_db: np.ndarray,
    speaker_emb: np.ndarray,
    centroids: np.ndarray,
) -> None:
    """
    Writes a binary .rvcfeat index file containing:
      - 4B Magic: "RVCF"
      - uint32 Version (1)
      - uint32 FeatureDim (128)
      - uint32 NumCentroids (64)
      - 64B Model Name (UTF-8 null-padded)
      - float32[128] Formant Bias Curve (dB)
      - float32[128] Speaker Embedding Vector g
      - float32[NumCentroids * 128] K-Means Feature Centroids
    """
    name_bytes = model_name.encode("utf-8")[:63].ljust(64, b"\x00")
    num_centroids, feat_dim = centroids.shape
    with open(output_path, "wb") as f:
        f.write(RVCFEAT_MAGIC)
        f.write(struct.pack("<III", RVCFEAT_VERSION, feat_dim, num_centroids))
        f.write(name_bytes)
        f.write(formant_bias_db.astype(np.float32).tobytes())
        f.write(speaker_emb.astype(np.float32).tobytes())
        f.write(centroids.astype(np.float32).tobytes())


def extract_from_user_file(input_path: Path):
    raise ValueError(
        "This legacy tool generates synthetic demo profiles; it cannot export RVC checkpoints. "
        "Load the original .pth and .index directly in the plugin."
    )


def generate_presets(output_dir: Path) -> list[Path]:
    """Generates the 3 built-in Virtual Singer RVC ONNX & .rvcfeat preset models."""
    output_dir.mkdir(parents=True, exist_ok=True)

    presets = [
        {
            "name": "Vocaloid_A_Lumina_Soprano",
            "formants": [
                (310.0, -2.5, 130.0),   # Reduced chest muddiness
                (790.0, 6.2, 170.0),    # Bright open vowel F1
                (2240.0, 7.4, 310.0),   # Clear forward F2
                (3380.0, 9.2, 420.0),   # Strong virtual diva singer's formant F3
                (4650.0, 6.8, 580.0),   # Crystalline upper harmonic ring F4
            ],
            "tilt": 0.45,
            "air_db": 6.5,
            "air_freq": 6800.0,
            "seed": 104729,
        },
        {
            "name": "Vocaloid_B_Nocturne_Alto",
            "formants": [
                (240.0, 5.8, 110.0),    # Warm fundamental & chest resonance
                (540.0, 7.5, 160.0),    # Deep rounded F1
                (1420.0, 5.2, 260.0),   # Warm dark-velvet F2
                (2580.0, 4.0, 380.0),   # Smooth mid-high transition F3
                (3650.0, -3.5, 520.0),  # Softened sibilance region
            ],
            "tilt": -1.35,
            "air_db": 2.2,
            "air_freq": 8500.0,
            "seed": 209715,
        },
        {
            "name": "Vocaloid_C_Kaguya_Whisper",
            "formants": [
                (280.0, -3.5, 140.0),   # Light intimate low-end
                (690.0, 4.8, 180.0),    # Soft neutral F1
                (2050.0, 8.2, 280.0),   # Intimate whisper presence F2
                (3120.0, 6.5, 390.0),   # Crisp articulation F3
                (5400.0, 8.8, 750.0),   # Breathy harmonic shimmer
            ],
            "tilt": 0.85,
            "air_db": 8.5,
            "air_freq": 5800.0,
            "seed": 314159,
        },
    ]

    generated_paths = []
    for p in presets:
        W_proj, formant_db, spk_emb, centroids = design_singer_profile(
            p["name"],
            p["formants"],
            harmonic_tilt_db_per_oct=p["tilt"],
            air_boost_db=p["air_db"],
            air_freq_hz=p["air_freq"],
            seed=p["seed"],
        )
        onnx_model = build_rvc_timbre_onnx_model(p["name"], W_proj, formant_db, spk_emb, centroids)
        onnx_path = output_dir / f"{p['name']}.onnx"
        feat_path = output_dir / f"{p['name']}.rvcfeat"

        onnx.save(onnx_model, str(onnx_path))
        write_rvcfeat_file(feat_path, p["name"], formant_db, spk_emb, centroids)

        # Verify inference with ONNX Runtime
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        dummy_mel = np.zeros((1, FEATURE_DIM, 8), dtype=np.float32)
        out_env, out_spk = sess.run(None, {"mel": dummy_mel})
        assert out_env.shape == (1, FEATURE_DIM, 8)
        assert out_spk.shape == (1, FEATURE_DIM)
        generated_paths.append(onnx_path)
        print(f"[OK] Generated & verified preset: {onnx_path.name} + {feat_path.name}")

    return generated_paths


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate legacy synthetic demo profiles; RVC checkpoint export is unsupported.")
    parser.add_argument("--generate-presets", action="store_true", help="Generate default Virtual Singer presets.")
    parser.add_argument("--input", type=str, help="Input RVC .pth, .index, or .onnx file.")
    parser.add_argument("--output-dir", type=str, default="models/presets", help="Output directory.")
    args = parser.parse_args()

    out_dir = Path(args.output_dir)
    if args.generate_presets:
        generate_presets(out_dir)
        return 0

    if args.input:
        in_path = Path(args.input)
        if not in_path.exists():
            print(f"[ERROR] Input file not found: {in_path}", file=sys.stderr)
            return 1
        out_dir.mkdir(parents=True, exist_ok=True)
        try:
            name, W_proj, formant_db, spk_emb, centroids = extract_from_user_file(in_path)
        except ValueError as exc:
            print(f"[ERROR] {exc}", file=sys.stderr)
            return 2
        onnx_model = build_rvc_timbre_onnx_model(name, W_proj, formant_db, spk_emb, centroids)
        onnx_path = out_dir / f"{name}.onnx"
        feat_path = out_dir / f"{name}.rvcfeat"
        onnx.save(onnx_model, str(onnx_path))
        write_rvcfeat_file(feat_path, name, formant_db, spk_emb, centroids)
        print(f"[OK] Exported {onnx_path} and {feat_path}")
        return 0

    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
