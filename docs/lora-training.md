# LoRA training

Nukleus trains LoRAs for SDXL checkpoints with a `lora.train` job (see
[the protocol](protocol-v3.md#training-a-lora-loratrain)). It does not install the
training stack: you install kohya-ss/sd-scripts and its Python environment yourself,
from the pinned steps below, and give the directory to the service with
`NUKLEUS_LORA_TRAINER`. Nukleus downloads no code; it runs the trainer as it runs
sd-cli, llama-server and ffmpeg, as a child of the service with the rights of the
service account.

The guide covers two setups with an AMD GPU:

- **Windows** with AMD's ROCm 7.2.1 PyTorch wheels. This is the installation that was
  actually done and checked (PyTorch sees an RX 7900 XTX), but AMD states that these
  wheels have "No ML training support" and no training job has been run there yet.
- **Linux** with ROCm, the setup that AMD supports for training. These steps are **not
  yet verified**: nobody has run them. Only the sd-scripts commit is aligned with the
  Windows installation; the other versions come from the ROCm 6.1 era and may need
  adjusting.

Nukleus does not block the operation on either platform: it is offered whenever
`NUKLEUS_LORA_TRAINER` names a working trainer. If training fails on Windows, train on
Linux and keep installing the resulting file as a LoRA companion.

Training and image generation share the GPU. A `lora.train` job waits for the queue
like any other job, unloads llama-server before it runs, and holds the GPU until it
ends: expect image jobs to wait, and 1500 steps at 1024 pixels to take tens of
minutes.

## Windows

Verified on Windows 11 with an AMD Radeon RX 7900 XTX. Run these in an administrator
PowerShell with network access; you do this, not Nukleus.

| Component | Version |
| --- | --- |
| Python | 3.12.10, installed for all users in `C:\Program Files\Python312` |
| kohya-ss/sd-scripts | commit `cb70b96` ("Merge pull request #2477 from kohya-ss/dev"), in `C:\Tools\sd-scripts` |
| venv | `C:\Tools\sd-scripts\venv` |
| ROCm wheels | AMD's ROCm 7.2.1 wheels from `https://repo.radeon.com/rocm/windows/rocm-rel-7.2.1/` |
| PyTorch | `2.9.1+rocm7.2.1` (torch and torchaudio 2.9.1+rocm7.2.1, torchvision 0.24.1+rocm7.2.1, cp312) |

Result on that machine: torch `2.9.1+rocm7.2.1`, `torch.cuda.is_available()` true, device
"AMD Radeon RX 7900 XTX"; transformers 5.17.0, diffusers 0.40.0, accelerate 1.15.0.

1. Install Python 3.12.10 for all users (`C:\Program Files\Python312`).
2. Get sd-scripts at the pinned commit (a shallow clone does not hold it):

   ```powershell
   git clone https://github.com/kohya-ss/sd-scripts.git C:\Tools\sd-scripts
   git -C C:\Tools\sd-scripts checkout cb70b96
   ```

3. Create the venv:
   `& "C:\Program Files\Python312\python.exe" -m venv C:\Tools\sd-scripts\venv`.
4. Install AMD's wheels from the repository above with the venv's `pip`, in this order:
   first `rocm_sdk_core`, `rocm_sdk_devel`, `rocm_sdk_libraries_custom` and
   `rocm-7.2.1.tar.gz`; then `torch-2.9.1+rocm7.2.1`, `torchaudio-2.9.1+rocm7.2.1` and
   `torchvision-0.24.1+rocm7.2.1` (cp312). Use the exact file names listed in that
   directory.
5. Install sd-scripts' requirements:
   `C:\Tools\sd-scripts\venv\Scripts\pip install -r C:\Tools\sd-scripts\requirements.txt`.
6. Check: `C:\Tools\sd-scripts\venv\Scripts\python -c "import torch; print(torch.__version__, torch.cuda.is_available())"`
   prints `2.9.1+rocm7.2.1 True`.
7. Fill the tokenizer cache as in step 6 of the Linux steps, into
   `C:\Tools\sd-scripts\tokenizers`.
8. Set `NUKLEUS_LORA_TRAINER=C:\Tools\sd-scripts` for the service and restart it.

No training job has been run on this setup yet.

## Linux (not yet verified)

### Versions

Everything below is pinned, but unverified. Other versions may work.

| Component | Version |
| --- | --- |
| Python | 3.10.12, built from source into `/opt/python-3.10.12` |
| PyTorch | 2.4.1+rocm6.1 |
| torchvision | 0.19.1+rocm6.1 |
| PyTorch wheel index | `https://download.pytorch.org/whl/rocm6.1` |
| kohya-ss/sd-scripts | commit `cb70b96`, as on Windows |
| Host | the amdgpu kernel driver of ROCm 6.1 or newer, and a GPU that ROCm supports (an RX 7900 XTX is `gfx1100`) |

The PyTorch wheels bring their own ROCm 6.1 libraries; the host needs the kernel driver
and access to `/dev/kfd` and `/dev/dri`, which the Nukleus installer gives the service
account through the `render` and `video` groups.

## Where to install it

The systemd unit sets `ProtectHome=true`, so the service cannot see `/home`. Install
the trainer, and the Python its `venv` is based on, outside `/home`, in directories the
`nukleus` account can read. The steps use `/opt`. A trainer under `/home` makes the
capability disappear without a message: the service checks the trainer at startup, finds
no Python, and does not report `lora.train`.

The unit also makes `/opt` read-only for the service. The trainer writes nothing there:
bytecode is turned off, and every cache goes to the service's own data directory
(`/var/lib/nukleus/data/nukleus/trainer-cache`).

## Steps

Run these as an administrator, with network access: this is the only step that
downloads anything, and you do it, not Nukleus.

1. Install what Python's build needs (a compiler, OpenSSL, zlib, libffi, SQLite and
   readline headers; on Debian and Ubuntu `build-essential libssl-dev zlib1g-dev
   libffi-dev libsqlite3-dev libreadline-dev`) and `git`.

2. Build Python 3.10.12 outside `/home`:

   ```sh
   cd /tmp
   curl -LO https://www.python.org/ftp/python/3.10.12/Python-3.10.12.tgz
   tar xf Python-3.10.12.tgz
   cd Python-3.10.12
   ./configure --prefix=/opt/python-3.10.12
   make -j"$(nproc)"
   sudo make install
   ```

3. Get sd-scripts at the pinned commit (a shallow clone does not hold it):

   ```sh
   sudo git clone https://github.com/kohya-ss/sd-scripts.git /opt/sd-scripts
   sudo git -C /opt/sd-scripts checkout cb70b96
   ```

4. Create the `venv` beside the scripts with that Python:

   ```sh
   sudo /opt/python-3.10.12/bin/python3.10 -m venv /opt/sd-scripts/venv
   ```

5. Install PyTorch for ROCm 6.1 from its wheel index, then sd-scripts' requirements:

   ```sh
   sudo /opt/sd-scripts/venv/bin/pip install torch==2.4.1 torchvision==0.19.1 \
       --index-url https://download.pytorch.org/whl/rocm6.1
   cd /opt/sd-scripts
   sudo ./venv/bin/pip install -r requirements.txt
   ```

6. Prepare the tokenizer cache once. sd-scripts fetches its two CLIP tokenizers from
   Hugging Face at each run unless they are in the cache directory; Nukleus runs the
   trainer offline (`HF_HUB_OFFLINE=1`, `TRANSFORMERS_OFFLINE=1`) and passes
   `--tokenizer_cache_dir /opt/sd-scripts/tokenizers`, so fill that directory now:

   ```sh
   sudo /opt/sd-scripts/venv/bin/python -c 'import os; from transformers import CLIPTokenizer; [CLIPTokenizer.from_pretrained(n).save_pretrained(os.path.join("/opt/sd-scripts/tokenizers", n.replace("/", "_"))) for n in ("openai/clip-vit-large-patch14", "laion/CLIP-ViT-bigG-14-laion2B-39B-b160k")]'
   ```

7. Make everything readable and executable by the service account:

   ```sh
   sudo chmod -R a+rX /opt/python-3.10.12 /opt/sd-scripts
   ```

8. Check the installation. The first command shows the interpreter the `venv` is based
   on, which must be outside `/home`; the second prints `2.4.1+rocm6.1 True`; the third
   lists the two tokenizers:

   ```sh
   sudo -u nukleus /opt/sd-scripts/venv/bin/python --version
   /opt/sd-scripts/venv/bin/python -c "import torch; print(torch.__version__, torch.cuda.is_available())"
   ls /opt/sd-scripts/tokenizers
   ```

## Configure the service

Name the trainer in `/etc/nukleus/nukleus.conf`, then restart the service. The service
checks the trainer once, when it starts, so restart it after installing or changing the
trainer too:

```sh
echo 'NUKLEUS_LORA_TRAINER=/opt/sd-scripts' | sudo tee -a /etc/nukleus/nukleus.conf
sudo systemctl restart nukleus
```

Check that the service reports the operation. Any WebSocket client does; with
[websocat](https://github.com/vi/websocat), type the first line and read the reply:

```sh
websocat ws://127.0.0.1:9384
{"type":"authenticate","version":3}
```

The `capabilities` reply lists `lora.train` in `operations`. If it does not, the journal
(`journalctl -u nukleus`) names `NUKLEUS_LORA_TRAINER` and says the directory holds no
`sdxl_train_network.py` with a working `venv` Python. Check that:

- `/opt/sd-scripts/sdxl_train_network.py` exists and `/opt/sd-scripts/venv/bin/python
  --version` runs as the `nukleus` account;
- neither the trainer nor the `home` of `/opt/sd-scripts/venv/pyvenv.cfg` is under
  `/home`;
- every directory on those paths is readable by `nukleus`.

## Using it

A client submits `lora.train` with the checkpoint, a name, one caption per image and the
images, and follows the progress (see [the protocol](protocol-v3.md#training-a-lora-loratrain)
for the options, the settings and the errors). The checkpoint must be an SDXL model, such
as WAI Illustrious. The trained file is installed as the LoRA companion
`<outputName>.safetensors`, ready for the `loras` option of `image.generate`.

A failed job returns the trainer's command, its exit code and the end of its error output
in a `generation-failed` message. A broken PyTorch or GPU stack (a missing HIP library,
no device) shows up there, not at startup, since the startup check does not import
PyTorch.
