"""A small ComfyUI client for texpack (story 5.8): starts ComfyUI headless from
the portable build, uploads an image, runs a workflow in API format and
fetches the output. Standard library only; ComfyUI's core nodes only (no
custom nodes), so the same workflow can be rebuilt by hand in its UI.
"""
import atexit
import json
import os
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

PORT = 8199


class ComfyError(RuntimeError):
    pass


def _http(url, data=None, headers=None, timeout=600):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        raise ComfyError(f"{url}: HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:2000]}")


class Comfy:
    def __init__(self, home, port=PORT, attach=False, deterministic=True):
        self.home = home                    # TEXPACK_HOME
        self.base = f"http://127.0.0.1:{port}"
        self.port = port
        self.client_id = uuid.uuid4().hex
        self.proc = None
        self.attach = attach
        self.deterministic = deterministic
        self.portable = os.path.join(home, 'ComfyUI_windows_portable')
        self.work = os.path.join(home, 'work')

    # -- server --------------------------------------------------------------
    def alive(self):
        try:
            _http(self.base + '/system_stats', timeout=3)
            return True
        except Exception:
            return False

    def start(self, log=print):
        if self.alive():
            log(f"comfy: attached to the server on port {self.port}")
            return
        if self.attach:
            raise ComfyError(f"no ComfyUI on port {self.port}")
        py = os.path.join(self.portable, 'python_embeded', 'python.exe')
        main = os.path.join(self.portable, 'ComfyUI', 'main.py')
        for p in (py, main):
            if not os.path.exists(p):
                raise ComfyError(f"{p} is missing: see README.md, Setup")
        os.makedirs(os.path.join(self.work, 'in'), exist_ok=True)
        os.makedirs(os.path.join(self.work, 'out'), exist_ok=True)
        args = [py, '-s', main, '--windows-standalone-build', '--listen', '127.0.0.1', '--port', str(self.port),
                '--disable-auto-launch', '--input-directory', os.path.join(self.work, 'in'),
                '--output-directory', os.path.join(self.work, 'out'), '--temp-directory', os.path.join(self.work, 'temp')]
        if self.deterministic:
            args.append('--deterministic')
        extra = os.path.join(self.home, 'extra_model_paths.yaml')     # models kept elsewhere (9.1)
        if os.path.exists(extra):
            args += ['--extra-model-paths-config', extra]
        logf = open(os.path.join(self.work, 'comfy.log'), 'wb')
        self.proc = subprocess.Popen(args, cwd=os.path.join(self.portable, 'ComfyUI'), stdout=logf, stderr=subprocess.STDOUT)
        atexit.register(self.stop)      # an exception, Ctrl-C or sys.exit still stops it
        log("comfy: starting the server ...")
        t0 = time.time()
        while time.time() - t0 < 300:
            if self.proc.poll() is not None:
                raise ComfyError(f"ComfyUI exited with {self.proc.returncode}; see {self.work}\\comfy.log")
            if self.alive():
                log(f"comfy: up after {time.time() - t0:.0f} s")
                return
            time.sleep(1)
        raise ComfyError("ComfyUI did not come up in 300 s")

    def stop(self):
        """Ends the server this object started (never one it attached to), with its process
        tree: a ComfyUI left running keeps the GPU busy and its VRAM taken."""
        if self.proc and self.proc.poll() is None:
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(self.proc.pid)], capture_output=True)
            try:
                self.proc.wait(20)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.stop()

    # -- jobs ----------------------------------------------------------------
    def upload(self, name, png_bytes):
        b = uuid.uuid4().hex
        body = b''.join([
            f'--{b}\r\nContent-Disposition: form-data; name="image"; filename="{name}"\r\nContent-Type: image/png\r\n\r\n'.encode(),
            png_bytes,
            f'\r\n--{b}\r\nContent-Disposition: form-data; name="overwrite"\r\n\r\ntrue\r\n--{b}--\r\n'.encode()])
        r = json.loads(_http(self.base + '/upload/image', body, {'Content-Type': f'multipart/form-data; boundary={b}'}))
        return r['name']

    def run(self, workflow, timeout=1800):
        """Runs a workflow in API format; returns the first output image's PNG bytes."""
        body = json.dumps({'prompt': workflow, 'client_id': self.client_id}).encode()
        pid = json.loads(_http(self.base + '/prompt', body, {'Content-Type': 'application/json'}))['prompt_id']
        t0 = time.time()
        while time.time() - t0 < timeout:
            h = json.loads(_http(self.base + '/history/' + pid))
            if pid in h:
                st = h[pid].get('status', {})
                if st.get('status_str') == 'error':
                    msgs = [m for m in st.get('messages', []) if m[0] == 'execution_error']
                    raise ComfyError(f"workflow failed: {json.dumps(msgs)[:2000]}")
                for out in h[pid]['outputs'].values():
                    for im in out.get('images', []):
                        q = urllib.parse.urlencode({'filename': im['filename'], 'subfolder': im['subfolder'], 'type': im['type']})
                        return _http(self.base + '/view?' + q)
                raise ComfyError("workflow finished without an image")
            time.sleep(0.25)
        raise ComfyError(f"workflow timed out after {timeout} s")


def build_workflow(image_name, p, seed, large=False):
    """Upscale with the 4x model, then (diffuse and skin modes) one img2img pass with
    ControlNet Tile. p: resolved class params plus 'prompt_text', 'upscaler',
    'checkpoint', 'controlnet'. large: encode and decode in tiles (images over
    about 1100 px)."""
    wf = {
        '1': {'class_type': 'LoadImage', 'inputs': {'image': image_name}},
        '2': {'class_type': 'UpscaleModelLoader', 'inputs': {'model_name': p['upscaler']}},
        '3': {'class_type': 'ImageUpscaleWithModel', 'inputs': {'upscale_model': ['2', 0], 'image': ['1', 0]}},
    }
    if p['mode'] == 'sprite':
        wf['12'] = {'class_type': 'PreviewImage', 'inputs': {'images': ['3', 0]}}
        return wf
    enc = 'VAEEncodeTiled' if large else 'VAEEncode'
    dec = 'VAEDecodeTiled' if large else 'VAEDecode'
    wf.update({
        '4': {'class_type': 'CheckpointLoaderSimple', 'inputs': {'ckpt_name': p['checkpoint']}},
        '5': {'class_type': 'CLIPTextEncode', 'inputs': {'text': p['prompt_text'], 'clip': ['4', 1]}},
        '6': {'class_type': 'CLIPTextEncode', 'inputs': {'text': p['negative'], 'clip': ['4', 1]}},
        '7': {'class_type': 'ControlNetLoader', 'inputs': {'control_net_name': p['controlnet']}},
        '8': {'class_type': 'ControlNetApplyAdvanced', 'inputs': {
            'positive': ['5', 0], 'negative': ['6', 0], 'control_net': ['7', 0], 'image': ['3', 0],
            'strength': p['cn_strength'], 'start_percent': 0.0, 'end_percent': 1.0, 'vae': ['4', 2]}},
        '9': {'class_type': enc, 'inputs': dict({'pixels': ['3', 0], 'vae': ['4', 2]}, **({'tile_size': 512, 'overlap': 64, 'temporal_size': 64, 'temporal_overlap': 8} if large else {}))},
        '10': {'class_type': 'KSampler', 'inputs': {
            'model': ['4', 0], 'seed': seed, 'steps': p['steps'], 'cfg': p['cfg'], 'sampler_name': 'dpmpp_2m',
            'scheduler': 'karras', 'positive': ['8', 0], 'negative': ['8', 1], 'latent_image': ['9', 0],
            'denoise': p['denoise']}},
        '11': {'class_type': dec, 'inputs': dict({'samples': ['10', 0], 'vae': ['4', 2]}, **({'tile_size': 512, 'overlap': 64, 'temporal_size': 64, 'temporal_overlap': 8} if large else {}))},
        '12': {'class_type': 'PreviewImage', 'inputs': {'images': ['11', 0]}},     # the temp folder, which ComfyUI empties at start
    })
    return wf
