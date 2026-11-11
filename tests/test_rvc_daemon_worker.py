"""Worker regressions. Run with the project's RVC runtime Python; no singer assets required."""
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.argv.append("--test-cli")
worker_path = Path(__file__).resolve().parents[1] / "tools" / "rvc_daemon_worker.py"
spec = importlib.util.spec_from_file_location("rvc_worker_under_test", worker_path)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)
sys.argv.remove("--test-cli")
np = worker.np
torch = worker.torch


class FakeNet:
    def __init__(self, *args, **kwargs):
        pass

    def load_state_dict(self, *args, **kwargs):
        pass

    def eval(self):
        return self

    def to(self, *args):
        return self

    def half(self):
        return self

    def float(self):
        return self

    def remove_weight_norm(self):
        pass


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="rvc-worker-", dir=Path(__file__).resolve().parents[1] / "artifacts")
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.engine = worker.RvcDaemonEngine()
        self.engine.ensure_base_models = mock.Mock(return_value=True)
        self.pth = self.folder / "voice.pth"
        self.pth.touch()
        models = types.ModuleType("infer.module.models")
        for name in ("SynthesizerTrnMs256NSFsid", "SynthesizerTrnMs256NSFsid_nono", "SynthesizerTrnMs768NSFsid", "SynthesizerTrnMs768NSFsid_nono"):
            setattr(models, name, FakeNet)
        self.module_patch = mock.patch.dict(sys.modules, {"infer.module.models": models})
        self.module_patch.start()
        self.addCleanup(self.module_patch.stop)
        self.checkpoint = {"config": [1, 1, 40000], "weight": {"emb_g.weight": torch.zeros((1, 1))}, "version": "v2", "f0": 1}
        self.load_patch = mock.patch.object(torch, "load", return_value=self.checkpoint)
        self.load_patch.start()
        self.addCleanup(self.load_patch.stop)

    def make_index(self, filename, dim=768, value=1.0, count=8):
        path = self.folder / filename
        index = worker.faiss.IndexFlatL2(dim)
        if count:
            index.add(np.full((count, dim), value, dtype=np.float32))
        worker.faiss.serialize_index(index).tofile(path)
        return str(path)

    def test_default_slots_are_empty_and_legacy_presets_require_manual_import(self):
        self.assertTrue(all(not slot.has_pth and not slot.file_path for slot in self.engine.slots))
        original = self.engine.load_slot(0, str(self.pth), "")
        request = struct.pack("<4sII", worker.REQ_MAGIC, worker.CMD_LOAD_PRESET, 8)
        request += struct.pack("<II", 0, 0)
        request += struct.pack("<4sII", worker.REQ_MAGIC, worker.CMD_SHUTDOWN, 0)
        response = io.BytesIO()
        with mock.patch.object(worker, "RvcDaemonEngine", return_value=self.engine), mock.patch.object(worker, "IPC_IN", io.BytesIO(request)), mock.patch.object(worker, "IPC_OUT", response), mock.patch.object(worker, "IS_TEST_CLI", False), mock.patch.object(self.engine, "load_slot") as load:
            worker.run_daemon_loop()
            load.assert_not_called()
        magic, kind, length = struct.unpack("<4sII", response.getvalue()[:12])
        self.assertEqual((magic, kind), (worker.RESP_MAGIC, worker.MSG_ERROR))
        self.assertIn(b"import a .pth model", response.getvalue()[12:12 + length])
        self.assertIs(self.engine.slots[0], original)

    def test_explicit_index_wins_and_is_reported(self):
        self.make_index("added_first.index", value=1.0)
        selected = self.make_index("z_selected.index", value=4.0)
        slot = self.engine.load_slot(0, str(self.pth), selected)
        self.assertEqual(slot.index_path, selected)
        self.assertEqual(slot.feature_dim, 768)
        self.assertTrue(np.all(slot.big_npy == 4.0))
        self.assertEqual(worker.resolve_pth_and_index(selected), (str(self.pth), selected))
        packed = self.engine.pack_slot_info(slot)
        self.assertEqual(len(packed), 32 + 64 + 512 + 64 + 512 + 512)
        self.assertEqual(packed[-512:].split(b"\0")[0].decode(), selected)

    def test_explicit_no_index_and_ambiguous_discovery(self):
        self.make_index("added_first.index")
        self.make_index("added_second.index")
        self.assertEqual(worker.resolve_pth_and_index(str(self.pth), ""), (str(self.pth), ""))
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            worker.resolve_pth_and_index(str(self.pth))
        other = self.folder / "other.pth"
        other.touch()
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            worker.resolve_pth_and_index(str(self.folder / "added_first.index"))

    def test_invalid_index_rejects_without_replacing_working_slot(self):
        good = self.make_index("good.index")
        original = self.engine.load_slot(0, str(self.pth), good)
        for bad, message in ((self.make_index("wrong.index", dim=256), "dimension"), (self.make_index("empty.index", count=0), "no trained")):
            with self.subTest(bad=bad):
                with self.assertRaisesRegex(ValueError, message):
                    self.engine.load_slot(0, str(self.pth), bad)
                self.assertIs(self.engine.slots[0], original)
                self.assertEqual(original.feature_dim, 768)
        with self.assertRaises(FileNotFoundError):
            self.engine.load_slot(0, str(self.pth), str(self.folder / "missing.index"))
        self.assertIs(self.engine.slots[0], original)

    def test_retrieval_cache_tracks_selected_index_and_file_replacement(self):
        cache = {}
        base = np.zeros((3, 768), dtype=np.float32)
        index_path = self.make_index("voice.index", value=1.0)
        first = self.engine.load_slot(0, str(self.pth), index_path)
        got = self.engine._retrieve_slot_features(first, base, 1.0, 1.0, cache)
        np.testing.assert_allclose(got, 1.0)
        alternate = self.make_index("other.index", value=2.0)
        second = self.engine.load_slot(0, str(self.pth), alternate)
        np.testing.assert_allclose(self.engine._retrieve_slot_features(second, base, 1.0, 1.0, cache), 2.0)
        self.make_index("voice.index", value=3.0)
        replaced = self.engine.load_slot(0, str(self.pth), index_path)
        np.testing.assert_allclose(self.engine._retrieve_slot_features(replaced, base, 1.0, 1.0, cache), 3.0)
        self.assertEqual(len(cache), 3)

    def test_load_slot_protocol_legacy_explicit_and_truncation(self):
        pth = str(self.pth).encode()
        old = struct.pack("<II", 1, len(pth)) + pth
        self.assertEqual(worker.unpack_load_slot_payload(old), (1, str(self.pth), None))
        self.assertEqual(worker.unpack_load_slot_payload(old + struct.pack("<I", 0)), (1, str(self.pth), ""))
        idx = "选择.index".encode()
        self.assertEqual(worker.unpack_load_slot_payload(old + struct.pack("<I", len(idx)) + idx)[2], "选择.index")
        for bad in (b"", old[:-1], old + b"\0", old + struct.pack("<I", 9)):
            with self.subTest(payload=bad):
                with self.assertRaises(ValueError):
                    worker.unpack_load_slot_payload(bad)

    def test_automation_uses_10ms_grid_with_context_and_reflect_padding(self):
        curves = np.tile(np.arange(31, dtype=np.float32)[:, None], (1, 4))
        got = worker.sample_automation_curve(curves, 2, 36, 320, 3200)
        expected = np.clip(np.arange(36) * 2 - 20, 0, 30)
        np.testing.assert_array_equal(got, expected)
        np.testing.assert_array_equal(worker.sample_automation_curve(curves[:1] + 0.7, 0, 5, 320, 3200), np.full(5, 0.7, dtype=np.float32))

    def test_rmvpe_uv_and_brief_voiced_islands_are_preserved(self):
        self.engine.device = torch.device("cpu")
        self.engine.is_half = False
        frames = 20
        features = torch.ones((1, frames, 768))
        self.engine.hubert = mock.Mock(return_value=types.SimpleNamespace(last_hidden_state=features, hidden_states=None))
        raw_f0 = np.zeros(frames * 2, dtype=np.float32)
        raw_f0[1:3] = 740.0
        raw_f0[8:30] = 739.3
        self.engine.rmvpe = types.SimpleNamespace(infer_from_audio=mock.Mock(return_value=raw_f0.copy()))
        high_zcr_audio = np.tile(np.array([0.3, -0.3], dtype=np.float32), 3200)
        entry, cached = self.engine._extract_or_get_cached_features(high_zcr_audio, 0, 1)
        self.assertFalse(cached)
        np.testing.assert_array_equal(entry[2], raw_f0)

    def test_protect_controls_only_unvoiced_retrieval_features(self):
        self.engine.device = torch.device("cpu")
        self.engine.is_half = False
        self.engine.slots[0].has_pth = True
        base = np.ones((32, 768), dtype=np.float32)
        f0 = np.tile(np.array([0, 220], dtype=np.float32), 32)
        features = (base, base[:, :256], f0, np.zeros(64, dtype=np.float32), {})
        self.engine._extract_or_get_cached_features = mock.Mock(return_value=(features, False))
        self.engine._retrieve_slot_features = mock.Mock(side_effect=lambda slot, feats, *args: feats * 10)
        captured = []

        def decode(slot, feats, pitch, pitchf, length, *args, **kwargs):
            captured.append(feats[0].cpu().numpy().copy())
            np.testing.assert_array_equal(pitchf[0].cpu().numpy() == 0, f0 == 0)
            return np.zeros(int(length[0]) * 400, dtype=np.float32), None, None, None, None

        self.engine._decode_single_slot = decode
        for protect, unvoiced_expected in ((0.0, 1.0), (0.33, 3.97), (0.5, 10.0)):
            self.engine.synthesize_chunk(
                np.ones(4000, dtype=np.float32) * 0.1,
                ab_morph=0, inject_depth=1, pitch_shift_st=0, f0_filter_radius=0,
                index_rate=1, protect=protect, slot_a_index_rate=1, slot_b_index_rate=1,
                rms_mix_rate=0.25, target_sr=40000,
            )
            np.testing.assert_allclose(captured[-1][::2], unvoiced_expected, rtol=1e-6)
            np.testing.assert_array_equal(captured[-1][1::2], 10.0)

    def test_runtime_assets_follow_runtime_root(self):
        engine = worker.RvcDaemonEngine()
        with mock.patch.object(worker, "RVC_RUNTIME_ROOT", str(self.folder)):
            with self.assertRaisesRegex(FileNotFoundError, "hubert_base") as error:
                engine.ensure_base_models()
        self.assertIn(str(self.folder), str(error.exception))

    def test_managed_state_contains_caches_and_overrides_inherited_paths(self):
        state = self.folder / "安装 状态"
        external = self.folder / "外部 共享缓存"
        path_variables = (
            "TEMP", "TMP", "TMPDIR", "JOBLIB_TEMP_FOLDER", "XDG_CACHE_HOME", "NUMBA_CACHE_DIR",
            "TORCH_HOME", "TORCH_EXTENSIONS_DIR", "TORCHINDUCTOR_CACHE_DIR", "PYTORCH_KERNEL_CACHE_PATH",
            "TRITON_CACHE_DIR", "CUDA_CACHE_PATH", "HF_HOME", "HF_HUB_CACHE", "HUGGINGFACE_HUB_CACHE",
            "TRANSFORMERS_CACHE", "HF_ASSETS_CACHE", "HUGGINGFACE_ASSETS_CACHE", "HF_XET_CACHE",
            "LIBROSA_CACHE_DIR", "MPLCONFIGDIR",
        )
        inherited = {name: str(external) for name in path_variables}
        inherited.update({"HF_HUB_OFFLINE": "0", "TRANSFORMERS_OFFLINE": "0", "HF_HUB_DISABLE_TELEMETRY": "0"})
        with mock.patch.dict(os.environ, inherited), mock.patch.object(tempfile, "tempdir", str(external)), mock.patch.object(sys, "dont_write_bytecode", False):
            self.assertEqual(worker.configure_runtime_state(str(state)), str(state))
            for name in path_variables:
                with self.subTest(variable=name):
                    target = Path(os.environ[name])
                    self.assertTrue(target.is_relative_to(state))
                    self.assertTrue(target.is_dir())
            self.assertEqual(tempfile.gettempdir(), str(state / "temp"))
            with tempfile.TemporaryFile() as temp:
                temp.write(b"managed temporary file")
            self.assertTrue(sys.dont_write_bytecode)
            self.assertFalse(external.exists())
            for name in ("HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE", "HF_HUB_DISABLE_TELEMETRY"):
                self.assertEqual(os.environ[name], "1")

    def test_portable_mode_preserves_user_cache_configuration(self):
        selected = self.folder / "用户缓存"
        with mock.patch.dict(os.environ, {"NUMBA_CACHE_DIR": str(selected), "HF_HOME": "external-hf-cache"}), mock.patch.object(tempfile, "tempdir", tempfile.tempdir), mock.patch.object(sys, "dont_write_bytecode", False):
            original = dict(os.environ)
            self.assertIsNone(worker.configure_runtime_state(None))
            self.assertEqual(dict(os.environ), original)
            self.assertTrue(selected.is_dir())
            self.assertFalse(sys.dont_write_bytecode)

    def test_managed_state_rejects_relative_paths(self):
        with self.assertRaisesRegex(ValueError, "absolute"):
            worker.configure_runtime_state("relative-state")

    def test_cuda_precision_matches_reference_compatibility_rules(self):
        profiles = [
            ("NVIDIA GeForce GTX 1060", (6, 1), 6, "cuda", False),
            ("NVIDIA GeForce GTX 1660 Ti", (7, 5), 6, "cuda", False),
            ("NVIDIA RTX 4070", (8, 9), 12, "cuda", True),
            ("NVIDIA GTX 960", (5, 2), 4, "cpu", False),
            ("Small GPU", (8, 6), 2, "cpu", False),
        ]
        for name, sm, gb, device_type, is_half in profiles:
            with self.subTest(name=name), mock.patch.object(torch.cuda, "is_available", return_value=True), mock.patch.object(torch.cuda, "device_count", return_value=1), mock.patch.object(torch.cuda, "get_device_capability", return_value=sm), mock.patch.object(torch.cuda, "get_device_name", return_value=name), mock.patch.object(torch.cuda, "get_device_properties", return_value=types.SimpleNamespace(total_memory=gb * 1024 ** 3)):
                device, half = worker.select_device_precision()
                self.assertEqual(device.type, device_type)
                self.assertEqual(half, is_half)


if __name__ == "__main__":
    unittest.main()
