"""Exercise real ONNX inference and mask postprocessing with small local graphs."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1] / "src" / "daemon" / "face-parsing.py"
# Keep any runtime telemetry/session files in disposable test storage.
RUNTIME_DIRECTORY = tempfile.TemporaryDirectory()
os.chdir(RUNTIME_DIRECTORY.name)

try:
    import numpy as np
    import onnx
    from onnx import helper, numpy_helper
    from PIL import Image
    import onnxruntime
except ImportError as error:
    print(f"SKIP: face parsing test dependencies are unavailable: {error}")
    raise SystemExit(0)

spec = importlib.util.spec_from_file_location(
    "face_parser", SOURCE
)
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)


class FaceParsingTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.input = self.root / "source.png"
        Image.new("RGB", (80, 40), (100, 120, 140)).save(self.input)
        self.output = self.root / "mask.png"
        self.options = dict(
            includedClasses=["hair"], dilatePixels=0, featherPixels=0, invertMask=False,
            preserveAspectRatio=True, outputAtSourceResolution=True,
            provider="CPUExecutionProvider",
        )

    def tearDown(self):
        self.directory.cleanup()

    def model(self, logits):
        # Identity copies constant logits; inference runs through ONNX Runtime.
        graph = helper.make_graph(
            [helper.make_node("Identity", ["constant"], ["logits"])], "fixture",
            [helper.make_tensor_value_info("pixel_values", onnx.TensorProto.FLOAT, [1, 3, 32, 32])],
            [helper.make_tensor_value_info("logits", onnx.TensorProto.FLOAT, list(logits.shape))],
            [numpy_helper.from_array(logits.astype(np.float32), "constant")],
        )
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        model.ir_version = 8
        path = self.root / "model.onnx"
        onnx.save(model, path)
        return path

    def test_source_resolution_regions_and_padding(self):
        logits = np.zeros((1, 19, 32, 32), dtype=np.float32)
        logits[:, 0] = 1
        # Padding occupies rows 0..7 and 24..31. Hair there must disappear.
        logits[:, 13, :8] = 10
        logits[:, 13, 12:20, 8:24] = 10
        parser.generate_mask(self.model(logits), self.input, self.output, self.options)
        with Image.open(self.output) as mask:
            self.assertEqual(mask.size, (80, 40))
            self.assertEqual(mask.mode, "L")
            self.assertEqual(mask.getpixel((40, 20)), 255)
            self.assertEqual(mask.getpixel((2, 2)), 0)
            baseline = np.asarray(mask).copy()
        self.options["invertMask"] = True
        parser.generate_mask(self.root / "model.onnx", self.input, self.output, self.options)
        with Image.open(self.output) as inverted:
            np.testing.assert_array_equal(np.asarray(inverted), 255 - baseline)

    def test_expansion_and_soft_edges(self):
        logits = np.zeros((1, 19, 32, 32), dtype=np.float32)
        logits[:, 0] = 1
        logits[:, 13, 12:20, 12:20] = 10
        model = self.model(logits)
        parser.generate_mask(model, self.input, self.output, self.options)
        with Image.open(self.output) as mask:
            before = np.count_nonzero(np.asarray(mask))
        self.options["dilatePixels"] = 4
        parser.generate_mask(model, self.input, self.output, self.options)
        with Image.open(self.output) as mask:
            self.assertGreater(np.count_nonzero(np.asarray(mask)), before)
        self.options["featherPixels"] = 3
        parser.generate_mask(model, self.input, self.output, self.options)
        with Image.open(self.output) as mask:
            values = np.asarray(mask)
            self.assertTrue(((values > 0) & (values < 255)).any())

    def test_empty_regions_and_wrong_model_fail(self):
        logits = np.zeros((1, 19, 32, 32), dtype=np.float32)
        logits[:, 0] = 1
        with self.assertRaisesRegex(ValueError, "No selected"):
            parser.generate_mask(self.model(logits), self.input, self.output, self.options)
        with self.assertRaisesRegex(ValueError, "19 face"):
            parser.generate_mask(self.model(logits[:, :2]), self.input, self.output, self.options)
        self.assertFalse(self.output.exists())

    def test_preprocessing_normalization(self):
        # Gray 100 has negative normalized channels, but positive unnormalized channels.
        Image.new("RGB", (80, 40), (100, 100, 100)).save(self.input)
        graph = helper.make_graph(
            [helper.make_node("ReduceMean", ["pixel_values"], ["mean"], axes=[1], keepdims=1),
             helper.make_node("Concat", ["zero"] + ["mean"] * 18, ["logits"], axis=1)],
            "normalization", [helper.make_tensor_value_info("pixel_values", onnx.TensorProto.FLOAT, [1, 3, 32, 32])],
            [helper.make_tensor_value_info("logits", onnx.TensorProto.FLOAT, [1, 19, 32, 32])],
            [numpy_helper.from_array(np.zeros((1, 1, 32, 32), dtype=np.float32), "zero")],
        )
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        model.ir_version = 8
        path = self.root / "model.onnx"
        onnx.save(model, path)
        self.options["includedClasses"] = ["background"]
        parser.generate_mask(path, self.input, self.output, self.options)
        with Image.open(self.output) as mask:
            self.assertEqual(mask.getextrema(), (255, 255))


if __name__ == "__main__":
    unittest.main()
