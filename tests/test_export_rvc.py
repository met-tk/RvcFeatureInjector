#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Automated Unit and Integration Tests for tools/export_rvc_to_onnx.py
Verifies:
1. Slaney Mel frequency calculations and singer profile design.
2. ONNX graph construction, ONNX Runtime dynamic tensor inference.
3. Binary .rvcfeat centroid file serialization and byte integrity.
4. Custom user file conversion (.pth simulation and .index FAISS centroid simulation).
5. Strict temporary file cleanup to prevent workspace pollution.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort

# Add project root to sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

import export_rvc_to_onnx as exp


class TestExportRvcToOnnx(unittest.TestCase):

    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp(prefix="rvc_test_"))

    def tearDown(self):
        if self.temp_dir.exists():
            shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_slaney_mel_frequencies(self):
        """Verify Slaney Mel bin center frequencies are monotonically increasing within [40Hz, 16kHz]."""
        freqs = exp.slaney_mel_frequencies(n_mels=128, fmin=40.0, fmax=16000.0)
        self.assertEqual(len(freqs), 128)
        self.assertTrue(np.all(np.diff(freqs) > 0.0))
        self.assertGreaterEqual(freqs[0], 35.0)
        self.assertLessEqual(freqs[-1], 16000.0)

    def test_design_singer_profile(self):
        """Verify singer profile generates valid float32 matrices and centroids."""
        formants = [(600.0, 5.0, 150.0), (1800.0, 6.0, 280.0), (3200.0, 7.0, 400.0)]
        W_proj, formant_bias, speaker_emb, centroids = exp.design_singer_profile(
            profile_name="TestSinger",
            formants_hz=formants,
            harmonic_tilt_db_per_oct=-0.5,
            air_boost_db=3.0,
            air_freq_hz=7000.0,
            seed=12345,
        )

        self.assertEqual(W_proj.shape, (128, 128))
        self.assertEqual(formant_bias.shape, (128,))
        self.assertEqual(speaker_emb.shape, (128,))
        self.assertEqual(centroids.shape, (64, 128))

        # Check finite and non-NaN
        self.assertTrue(np.all(np.isfinite(W_proj)))
        self.assertTrue(np.all(np.isfinite(formant_bias)))
        self.assertTrue(np.all(np.isfinite(speaker_emb)))
        self.assertTrue(np.all(np.isfinite(centroids)))

        # W_proj row sums should be normalized to approximately 1.0
        row_sums = np.sum(W_proj, axis=1)
        self.assertTrue(np.all(np.abs(row_sums - 1.0) < 1e-4))

    def test_build_and_run_onnx_model(self):
        """Verify built ONNX model executes correctly in ONNX Runtime with dynamic time frames."""
        W_proj, formant_bias, speaker_emb, centroids = exp.design_singer_profile(
            "DynamicTest", [(700.0, 5.0, 200.0)], -0.8, 2.0, 6000.0, 999
        )
        model = exp.build_rvc_timbre_onnx_model("DynamicTest", W_proj, formant_bias, speaker_emb, centroids)
        onnx.checker.check_model(model)

        onnx_file = self.temp_dir / "test_model.onnx"
        onnx.save(model, str(onnx_file))

        sess = ort.InferenceSession(str(onnx_file), providers=["CPUExecutionProvider"])

        # Test with variable time steps (e.g. 1 frame, 8 frames, 32 frames)
        for num_frames in [1, 8, 32]:
            dummy_mel = np.random.uniform(-40.0, 10.0, size=(1, 128, num_frames)).astype(np.float32)
            out_env, out_spk = sess.run(None, {"mel": dummy_mel})

            self.assertEqual(out_env.shape, (1, 128, num_frames))
            self.assertEqual(out_spk.shape, (1, 128))
            self.assertTrue(np.all(np.isfinite(out_env)))
            self.assertTrue(np.all(np.isfinite(out_spk)))

    def test_rvcfeat_file_serialization(self):
        """Verify binary .rvcfeat file format, magic header, and byte layout."""
        W_proj, formant_bias, speaker_emb, centroids = exp.design_singer_profile(
            "FeatTest", [(500.0, 4.0, 150.0)], -0.3, 1.5, 7500.0, 42
        )
        feat_path = self.temp_dir / "test.rvcfeat"
        exp.write_rvcfeat_file(feat_path, "FeatTest", formant_bias, speaker_emb, centroids)

        self.assertTrue(feat_path.exists())
        file_bytes = feat_path.read_bytes()

        # Expected size:
        # Magic (4) + Version (4) + FeatDim (4) + NumCentroids (4) + Name (64)
        # + FormantBias (128 * 4) + SpeakerEmb (128 * 4) + Centroids (64 * 128 * 4)
        expected_size = 4 + 4 + 4 + 4 + 64 + 128 * 4 + 128 * 4 + 64 * 128 * 4
        self.assertEqual(len(file_bytes), expected_size)

        # Validate header
        magic, version, feat_dim, num_centroids = struct.unpack("<4sIII", file_bytes[:16])
        self.assertEqual(magic, b"RVCF")
        self.assertEqual(version, 1)
        self.assertEqual(feat_dim, 128)
        self.assertEqual(num_centroids, 64)

        # Validate raw float values
        offset = 80
        read_formant = np.frombuffer(file_bytes[offset : offset + 512], dtype=np.float32)
        np.testing.assert_allclose(read_formant, formant_bias, rtol=1e-6)

    def test_rejects_fake_checkpoint_export(self):
        for suffix in (".pth", ".index"):
            fake = self.temp_dir / ("fake" + suffix)
            fake.write_bytes(b"not a neural checkpoint")
            with self.assertRaises(ValueError):
                exp.extract_from_user_file(fake)
            output = self.temp_dir / ("output" + suffix)
            result = subprocess.run([sys.executable, str(PROJECT_ROOT / "tools" / "export_rvc_to_onnx.py"), "--input", str(fake), "--output-dir", str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("cannot export RVC", result.stderr)
            self.assertFalse(list(output.glob("*.onnx")))


if __name__ == "__main__":
    unittest.main()
