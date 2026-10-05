"""Run sd.cpp's official face_detect.py with strict single-face validation."""
import runpy
import sys

from insightface.app import FaceAnalysis


class AmbiguousIdentity(Exception):
    pass


original_get = FaceAnalysis.get
previous_image = None
previous_faces = None


def single_face_get(self, *args, **kwargs):
    global previous_image, previous_faces
    image = args[0] if args else kwargs["img"]
    # The official extractor retries detection sizes on the same image. Check
    # an undetected reference only after those retries, when the image changes.
    if previous_image is not None and image is not previous_image and not previous_faces:
        raise ValueError("No face detected in input image pool")
    faces = original_get(self, *args, **kwargs)
    if len(faces) > 1:
        raise AmbiguousIdentity("Multiple faces detected; provide a crop containing one face.")
    previous_image = image
    previous_faces = faces
    return faces


FaceAnalysis.get = single_face_get
script, image_directory = sys.argv[1:]
sys.argv = [script, image_directory]
try:
    runpy.run_path(script, run_name="__main__")
    if previous_image is not None and not previous_faces:
        raise ValueError("No face detected in input image pool")
except AmbiguousIdentity as error:
    print(str(error), file=sys.stderr)
    sys.exit(11)
except ValueError as error:
    if str(error) == "No face detected in input image pool":
        print(str(error), file=sys.stderr)
        sys.exit(10)
    raise
