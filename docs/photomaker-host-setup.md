# PhotoMaker V2 identity extractor on the service host

This configures the existing Linux/systemd Kira Service identity-export API.
Run these commands on the **service host**, not on the Canvas computer.
Kira Service must already be installed, including its kira-service account.

The extractor is separate from the PhotoMaker generation weights: exporting
id_embeds.bin requires InsightFace face-analysis assets, not
photomaker-v2.safetensors. The Manager's existing Add model workflow installs
PhotoMaker weights but does not install Python or configure extraction.

## 1. Prepare the Python environment

Use a Python 3.11 or 3.12 interpreter with its development headers and venv
support, plus a C/C++ compiler. The commands below use python3.11; substitute
python3.12 consistently if that is the interpreter provided by your distribution.
InsightFace 0.7.3 builds a native extension. Do not use the system pip environment.

```sh
sudo install -d -m 0755 /opt/kira-photomaker
sudo python3.11 -m venv /opt/kira-photomaker/venv
sudo /opt/kira-photomaker/venv/bin/python -m pip install --upgrade pip setuptools wheel
sudo /opt/kira-photomaker/venv/bin/python -m pip install 'numpy<2' Cython
sudo /opt/kira-photomaker/venv/bin/python -m pip install torch --index-url https://download.pytorch.org/whl/cpu
sudo /opt/kira-photomaker/venv/bin/python -m pip install --no-build-isolation 'numpy<2' diffusers safetensors Pillow onnxruntime insightface==0.7.3
sudo /opt/kira-photomaker/venv/bin/python -m pip check
```

The NumPy constraint keeps the legacy extractor environment on the NumPy 1.x
ABI. Install CPU ONNX Runtime, not onnxruntime-gpu: the upstream extractor
explicitly selects CPUExecutionProvider. CPU PyTorch wheels above are intended
for supported Linux hosts; consult the PyTorch installer if your architecture
has no matching wheel. The commands are setup guidance, not a tested dependency
lock for every distribution.

## 2. Install the official extraction script

Use the stable-diffusion.cpp source checkout corresponding to your deployed
sd-cli. Replace the two source paths below with your existing checkouts:

```sh
KIRA_SD_SOURCE=/absolute/path/to/stable-diffusion.cpp
KIRA_SERVICE_SOURCE=/absolute/path/to/kira-service
test -r "$KIRA_SD_SOURCE/scripts/face_detect.py"
test -r "$KIRA_SERVICE_SOURCE/src/daemon/photomaker-export.py"
sudo install -m 0644 "$KIRA_SD_SOURCE/scripts/face_detect.py" /opt/kira-photomaker/face_detect.py
sudo install -m 0644 "$KIRA_SERVICE_SOURCE/src/daemon/photomaker-export.py" /opt/kira-photomaker/export-check.py
```

The second file is used only for the command-line smoke test. The daemon already
embeds the same strict single-face guard, so it does not need that copy.
Use /opt rather than a checkout under /home: the installed systemd unit enables
ProtectHome. Do not weaken its sandbox or make the service run as root.

## 3. Provision the InsightFace assets as the service account

Initialize the same CPU detector/recognizer used by the official script:

```sh
sudo -u kira-service -H env HOME=/var/lib/kira-service XDG_CACHE_HOME=/var/lib/kira-service/cache \
  /opt/kira-photomaker/venv/bin/python -c \
  'from insightface.app import FaceAnalysis; app = FaceAnalysis(providers=["CPUExecutionProvider"], allowed_modules=["detection", "recognition"]); app.prepare(ctx_id=0, det_size=(640, 640)); assert "recognition" in app.models; print("PhotoMaker face analysis is ready")'
```

The first run downloads the default buffalo_l package into
/var/lib/kira-service/.insightface/models/buffalo_l. Run this while the host
has network access, before relying on offline export. If automatic retrieval
fails, provision that package manually according to the
[InsightFace model instructions](https://github.com/deepinsight/insightface/tree/v0.7/python-package),
retaining detection and recognition ONNX files and service-account ownership.
Downloading assets as your login user does not populate the service's cache.
The upstream pretrained assets have their own usage terms; see that source.

## 4. Smoke-test extraction with one face

Choose a full image or crop containing exactly one clear face. This Bash
subshell uses a disposable directory; it retains your original image and removes
the test input/output when done. Replace the source image path first:

```sh
(
  set -eu
  KIRA_FACE_IMAGE=/absolute/path/to/single-face.png
  KIRA_CHECK_DIR=$(sudo -u kira-service -H mktemp -d /var/lib/kira-service/photomaker-check.XXXXXX)
  trap 'sudo rm -rf -- "$KIRA_CHECK_DIR"' EXIT
  sudo install -o kira-service -g kira-service -m 0600 "$KIRA_FACE_IMAGE" "$KIRA_CHECK_DIR/input.png"
  sudo -u kira-service -H env HOME=/var/lib/kira-service XDG_CACHE_HOME=/var/lib/kira-service/cache \
    /opt/kira-photomaker/venv/bin/python /opt/kira-photomaker/export-check.py \
    /opt/kira-photomaker/face_detect.py "$KIRA_CHECK_DIR"
  sudo -u kira-service test -s "$KIRA_CHECK_DIR/id_embeds.bin"
  echo "Identity extraction succeeded"
)
```

Repeat with a no-face image and a multiple-face image: expect failure, exit code
10 or 11 respectively, and no identity output. This exercises the same guard as
the daemon. It does not submit a generation job or export your test identity to
persistent storage.

## 5. Configure and restart Kira Service

Edit the existing configuration, preserving its listen/port/token settings:

```sh
sudoedit /etc/kira-service/kira-service.conf
```

Add or replace these entries, with no duplicate keys:

```ini
KIRA_PHOTOMAKER_PYTHON=/opt/kira-photomaker/venv/bin/python
KIRA_PHOTOMAKER_FACE_DETECT=/opt/kira-photomaker/face_detect.py
HOME=/var/lib/kira-service
XDG_CACHE_HOME=/var/lib/kira-service/cache
```

```sh
sudo systemctl restart kira-service
sudo systemctl status kira-service --no-pager
sudo journalctl -u kira-service -n 50 --no-pager
```

An environment-file edit needs a restart, not daemon-reload. Keep the
configuration private: it also points to authentication settings.
The regular installer preserves an existing valid configuration during updates.

Reconnect your SDK client. capabilities.operations should include
photomaker.export and ServiceCapabilities.photoMakerIdentityExport should be
true. Submit an export through exportPhotoMakerIdentity(imageBytes); successful
photoMakerIdentityExportFinished delivers identityData containing id_embeds.bin.
No Canvas identity-export interface is implemented yet.

The advertised capability currently checks only the interpreter and script paths.
It is not proof that dependencies/assets or real inference work. The smoke test
above plus an export over the service API are the readiness checks.

## Troubleshooting

- No photomaker.export capability: verify both environment entries, file
  readability/executable permissions for kira-service, restart and reconnect.
- ModuleNotFoundError: install the dependency with the configured venv interpreter.
  Installing it with another python or pip will not fix this environment.
- NumPy/Cython build errors: check the Python development headers, compiler and
  NumPy constraint; rerun pip check. A newer system Python may need a separate
  3.11/3.12 interpreter.
- Permission/cache errors: run initialization as kira-service with the HOME/cache
  values above. Keep assets under /var/lib/kira-service, not /root or /home.
- No face / multiple faces: use a clear single-face crop; these are explicit
  input errors, not missing PhotoMaker weights.
- Test works but API fails: check the running service's configured interpreter,
  script and environment, then inspect the job-scoped SDK error message.

## Service Manager integration

This runtime setup belongs in a dedicated PhotoMaker extraction setup/status
workflow, separate from Add model. Such a workflow should show dependency/model
readiness, initialize assets under the service account, verify extraction and
configure the service through an explicit local administrative installation step.
The current local Manager has no such action, and its model download endpoint
must not be repurposed to execute arbitrary scripts or pip commands.

References:

- [Official face_detect.py](https://github.com/leejet/stable-diffusion.cpp/blob/master/scripts/face_detect.py)
- [InsightFace 0.7 FaceAnalysis](https://github.com/deepinsight/insightface/blob/v0.7/python-package/insightface/app/face_analysis.py)
- [PyTorch installation](https://pytorch.org/get-started/locally/)
