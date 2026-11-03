#!/usr/bin/env python3
"""Real Qwen restart regression. Stop other huge-model servers before running.

Uses synthetic history and a fresh output directory, never production caches.
Any model-specific environment (for example the MTP sidecar) is inherited.
"""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=8011)
    parser.add_argument('--budget-mb', type=int, default=512)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    url = f'http://127.0.0.1:{args.port}'
    tools = [{'type': 'function', 'function': {
        'name': 'synthetic_read', 'description': 'Read synthetic fixtures',
        'parameters': {'type': 'object', 'properties': {}}}}]
    history = [
        {'role': 'system', 'content': 'Synthetic restart test. Reply exactly READY. ' +
         'Stable instructions preserve original files. ' * 120},
        {'role': 'user', 'content': 'Synthetic historical evidence. ' +
         'Only synthetic information is included in this fixture. ' * 500},
        {'role': 'assistant', 'content': None, 'tool_calls': [{
            'id': 'call_fixture_1', 'type': 'function',
            'function': {'name': 'synthetic_read', 'arguments': '{}'}}]},
        {'role': 'tool', 'tool_call_id': 'call_fixture_1',
         'content': 'Synthetic tool result only.'},
        {'role': 'assistant', 'content': 'Recorded fixture result.'}]
    results = []
    for version in (1, 2):
        with (args.output / f'server-{version}.log').open('w') as log:
            process = subprocess.Popen([
                str(args.server.resolve()), '-m', str(args.model.resolve()), '--mtp',
                '--ctx', '16384', '--prefill-chunk', '2048', '--host', '127.0.0.1',
                '--port', str(args.port), '--kv-disk-dir', str(args.output.resolve() / 'cache'),
                '--kv-disk-space-mb', str(args.budget_mb)], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 240
                while True:
                    if process.poll() is not None:
                        raise RuntimeError('test server exited before readiness')
                    try:
                        with urllib.request.urlopen(url + '/v1/models', timeout=2) as response:
                            response.read()
                        break
                    except (OSError, urllib.error.URLError):
                        if time.monotonic() > deadline:
                            raise RuntimeError('test server readiness timed out')
                        time.sleep(1)
                payload = {'model': 'qwen3.8-flash-next-chat', 'think': False,
                           'temperature': 0, 'max_tokens': 64, 'tools': tools,
                           'messages': history + [{'role': 'user', 'content':
                               f'[Message sent at | Date: 2026-11-03 | Time: 17:0{version}:00 | '
                               'Time zone: Asia/Seoul (UTC+09:00)]\nReply exactly READY.'}]}
                request = urllib.request.Request(url + '/v1/chat/completions',
                    data=json.dumps(payload).encode(), headers={'Content-Type': 'application/json'})
                started = time.monotonic()
                with urllib.request.urlopen(request, timeout=300) as response:
                    reply = json.load(response)
                assert reply['choices'][0]['finish_reason'] == 'stop', reply
                assert reply['choices'][0]['message']['content'].strip() == 'READY', reply
                usage = reply['usage']
                cached = usage['prompt_tokens_details']['cached_tokens']
                if version == 1:
                    assert cached == 0, usage
                else:
                    # A fresh last-user clock must not throw away older history.
                    # The stable system-only prefix is less than 20% of this fixture.
                    assert cached > usage['prompt_tokens'] * 0.9, usage
                row = {'version': version, 'seconds': time.monotonic() - started, 'usage': usage}
                results.append(row)
                (args.output / 'results.json').write_text(json.dumps(results, indent=2))
                print(json.dumps(row), flush=True)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                    process.wait(timeout=90)


if __name__ == '__main__':
    main()
