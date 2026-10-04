"""Run sd.cpp's official face_detect.py with strict single-face validation."""
import runpy
import sys

from insightface.app import FaceAnalysis


class AmbiguousIdentity(Exception):
    pass


original_get = FaceAnalysis.get


def single_face_get(self, *args, **kwargs):
    faces = original_get(self, *args, **kwargs)
    if len(faces) > 1:
        raise AmbiguousIdentity("Multiple faces detected; provide a crop containing one face.")
    return faces


FaceAnalysis.get = single_face_get
script, image_directory = sys.argv[1:]
sys.argv = [script, image_directory]
try:
    runpy.run_path(script, run_name="__main__")
except AmbiguousIdentity as error:
    print(str(error), file=sys.stderr)
    sys.exit(11)
except ValueError as error:
    if str(error) == "No face detected in input image pool":
        print(str(error), file=sys.stderr)
        sys.exit(10)
    raise
