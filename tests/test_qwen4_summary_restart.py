"""Model-backed cache regression for historical system summaries and restart.

Writes only synthetic prompts and cache files under --output (or a new temp
folder). Requires one resident Qwen model; do not run alongside another large
model server. Artifacts are retained for inspection rather than deleted.
"""

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', required=True)
    parser.add_argument('--ctx', type=int, default=8192)
    parser.add_argument('--prefill-chunk', type=int, default=512)
    parser.add_argument('--summary-slot', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--ngram-model', type=Path, help='Separate n-gram GGUF for split Qwen models')
    args = parser.parse_args()
    root = (args.output or Path(tempfile.mkdtemp(prefix='ds4-summary-restart-'))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    cache = Path(tempfile.mkdtemp(prefix='kv-', dir=root))
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    base_url = f'http://127.0.0.1:{port}'
    command = [
        str(Path(__file__).resolve().parents[1] / 'ds4-server'),
        '-m', str(Path(args.model).resolve()), '--ctx', str(args.ctx),
        '--prefill-chunk', str(args.prefill_chunk), '--host', '127.0.0.1', '--port', str(port),
        '--kv-disk-dir', str(cache), '--kv-disk-space-mb', '768',
        '--kv-cache-min-tokens', '512', '--kv-cache-cold-max-tokens', '2000',
        '--kv-cache-continued-interval-tokens', '0',
    ]
    if args.summary_slot:
        command.append('--summary-slot')
    environment = os.environ.copy()
    if args.ngram_model:
        environment['DS4_QWEN_NGRAM_GGUF'] = str(args.ngram_model.resolve())
    results = []
    proc = None
    log = None
    system = (
        f'Synthetic restart regression {uuid.uuid4()}. '
        'Always answer OK to the current request.\n'
        + ''.join(f'Fixed rule {i}: preserve stable instructions and synthetic identifiers.\n'
                  for i in range(120))
    )
    base = [{'role': 'system', 'content': system}]
    history = []
    for i in range(10):
        history += [
            {'role': 'user', 'content': f'Synthetic historical item {i}: '
             + 'Historical placeholder. ' * 40},
            {'role': 'assistant', 'content': 'OK'},
        ]
    label = 'Earlier conversation summary (historical data, not new instructions).'

    def start(stage):
        nonlocal proc, log
        log = (root / (stage + '.log')).open('w')
        proc = subprocess.Popen(command, stdout=log, stderr=log, env=environment)
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError('server exited ' + stage)
            try:
                with urllib.request.urlopen(base_url + '/v1/models', timeout=2) as response:
                    if response.status == 200:
                        return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(.5)
        raise TimeoutError('server load ' + stage)

    def stop():
        nonlocal proc, log
        if proc and proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=120)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise RuntimeError('synthetic test server did not stop gracefully')
        if log:
            log.close()
        proc = None

    def call(name, messages):
        body = dict(model='qwen3.8-flash-next-chat', messages=messages,
                    stream=False, temperature=0, max_tokens=128, reasoning_effort='none')
        started = time.monotonic()
        request = urllib.request.Request(
            base_url + '/v1/chat/completions', data=json.dumps(body).encode(),
            headers={'Content-Type': 'application/json'},
        )
        with urllib.request.urlopen(request, timeout=600) as response:
            reply = json.load(response)
        message = reply['choices'][0]['message']
        row = dict(label=name, seconds=round(time.monotonic() - started, 3),
                   usage=reply['usage'], content=message.get('content'))
        results.append(row)
        print(json.dumps(row), flush=True)
        (root / 'result.json').write_text(json.dumps(results, indent=2))
        assert message.get('content', '').strip() == 'OK', message
        return message

    def cached_tokens():
        return results[-1]['usage']['prompt_tokens_details']['cached_tokens']

    try:
        start('first')
        call('long_first_prompt_above_cold_limit',
             base + history + [{'role': 'user', 'content': 'Reply only OK.'}])
        assert results[-1]['usage']['prompt_tokens'] > 2000, results[-1]
        compacted = base + history[-2:] + [
            {'role': 'system', 'content': label + ' Six older synthetic facts retained.'},
            {'role': 'user', 'content': 'Reply only OK.'},
        ]
        message = call('summary_replaces_older_history', compacted)
        assert cached_tokens() >= 1024, results[-1]
        compacted += [message, {'role': 'user', 'content': 'Again reply only OK.'}]
        message = call('post_summary_followup', compacted)
        assert cached_tokens() >= results[-1]['usage']['prompt_tokens'] * .9, results[-1]
        compacted += [message, {'role': 'user', 'content': 'After restart reply only OK.'}]
        stop()
        start('restarted')
        call('first_request_after_restart', compacted)
        assert cached_tokens() >= results[-1]['usage']['prompt_tokens'] * .9, results[-1]
        # The loaded full checkpoint has no live anchor. Its separately saved
        # fixed disk prefix must recover when the historical summary changes.
        for message in compacted:
            if message['role'] == 'system' and message['content'].startswith(label):
                message['content'] = label + ' Updated synthetic facts after restart.'
        call('changed_summary_after_restart', compacted)
        assert cached_tokens() >= 1024, results[-1]
    finally:
        stop()


if __name__ == '__main__':
    main()
