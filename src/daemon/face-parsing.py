"""Generate a grayscale face mask using a local SegFormer/CelebAMask-HQ ONNX model."""
import json
import sys

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageFilter, ImageOps


LABELS = (
    "background", "skin", "nose", "eye_g", "l_eye", "r_eye", "l_brow", "r_brow",
    "l_ear", "r_ear", "mouth", "u_lip", "l_lip", "hair", "hat", "ear_r",
    "neck_l", "neck", "cloth",
)


def generate_mask(model_path, image_path, output_path, options):
    with Image.open(image_path) as source:
        if not (0 < source.width <= 4096 and 0 < source.height <= 4096):
            raise ValueError("Source dimensions must be between 1 and 4096 pixels")
        image = ImageOps.exif_transpose(source).convert("RGB")
    session = ort.InferenceSession(model_path, providers=[options["provider"]])
    inputs = session.get_inputs()
    if len(inputs) != 1 or inputs[0].type != "tensor(float)" or len(inputs[0].shape) != 4:
        raise ValueError("Expected one float32 NCHW image input")
    shape = inputs[0].shape
    height = shape[2] if isinstance(shape[2], int) and shape[2] > 0 else 512
    width = shape[3] if isinstance(shape[3], int) and shape[3] > 0 else 512
    if not (0 < height <= 2048 and 0 < width <= 2048):
        raise ValueError("Unsupported model input dimensions")
    if options["preserveAspectRatio"]:
        scale = min(width / image.width, height / image.height)
        fitted = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    else:
        fitted = (width, height)
    left, top = (width - fitted[0]) // 2, (height - fitted[1]) // 2
    canvas = Image.new("RGB", (width, height), (124, 116, 104))
    canvas.paste(image.resize(fitted, Image.Resampling.BILINEAR), (left, top))
    pixels = np.asarray(canvas, dtype=np.float32) / 255.0
    pixels = (pixels - np.array([0.485, 0.456, 0.406], dtype=np.float32)) / np.array(
        [0.229, 0.224, 0.225], dtype=np.float32
    )
    logits = session.run(None, {inputs[0].name: pixels.transpose(2, 0, 1)[None]})[0]
    if (logits.ndim != 4 or logits.shape[0] != 1 or logits.shape[1] != len(LABELS)
            or not np.isfinite(logits).all()):
        raise ValueError("Expected finite logits for 19 face parsing classes")
    scores = np.stack([
        np.asarray(Image.fromarray(channel).resize((width, height), Image.Resampling.BILINEAR))
        for channel in logits[0]
    ])
    labels = scores.argmax(axis=0)[top:top + fitted[1], left:left + fitted[0]]
    included = [LABELS.index(name) for name in options["includedClasses"]]
    selected = np.isin(labels, included)
    if not selected.any():
        raise ValueError("No selected face regions were found in the image")
    mask = Image.fromarray(selected.astype(np.uint8) * 255).resize(image.size, Image.Resampling.NEAREST)
    radius = options["dilatePixels"]
    if radius:
        mask = mask.filter(ImageFilter.MaxFilter(2 * radius + 1))
    if options["featherPixels"]:
        mask = mask.filter(ImageFilter.GaussianBlur(options["featherPixels"]))
    if options["invertMask"]:
        mask = ImageOps.invert(mask)
    if not options["outputAtSourceResolution"]:
        mask = mask.resize(fitted, Image.Resampling.BILINEAR)
    mask.save(output_path, format="PNG")


if __name__ == "__main__":
    try:
        generate_mask(*sys.argv[1:4], json.loads(sys.argv[4]))
    except Exception as error:
        print(f"Face parsing failed: {error}", file=sys.stderr)
        sys.exit(1)
