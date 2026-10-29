# Personal Dstar branch

Base: `paperniuk/ds4` commit `37a5f4a4847cbf08db0a6e7ae638aec7924edd38`
(`m1-flash-next` lineage). The original upstream is `antirez/ds4`.

## Changes

- Recover an ordinary Qwen OpenAI chat cache when sampled and re-tokenized
  tokens differ but the bounded decoded live tail is an exact byte prefix of
  the new prompt. Only the new suffix is tokenized; the sampled KV frontier
  remains intact. This path requires an unchanged token prefix, matching
  vision identity, at most 512 live-tail tokens, at most 4096 incoming-tail
  tokens, and no image crossing the comparison boundary. It does not accept
  whitespace normalization or apply to other protocols/model syntaxes.
- Emit count-only `atomic-progress` SSE comments during prefill. They contain
  processed, total, cached-token count and elapsed milliseconds. Standard SSE
  clients ignore comments. Percentages describe uncached input processing,
  not model loading, reasoning, generation completion or an ETA.
- Add cache diagnostics with counts, bounded prefix fingerprints and tail
  classifications. No prompt/output text, token IDs, credentials, headers,
  paths or tool arguments are included in the added diagnostics. Whitespace
  classification is diagnostic only and never authorizes cache reuse.

## Verification

`make ds4-server ds4_test` and `./ds4_test --server` cover the server changes,
including exact decoded suffixes, rejected whitespace-only differences,
embedded NULs, protocol/vision safety gates, and metadata-only diagnostics.
Run `make test-frontends` for the broader model-independent frontend checks.

Earlier live validation on the same source observed a 40,406-token retained
frontier with a 64-token uncached suffix; prefill completed in about 2.14 s.
The new comment stream was also observed as a native app reading-input
percentage. These are individual local observations, not general throughput
or output-quality claims. Large-model inference is not repeated when merely
organizing this branch; model files and caches are not part of the repository.

Atomic Chat integration is maintained independently in
[Young-uk/Atomic-Chat](https://github.com/Young-uk/Atomic-Chat).


## Append-only effort experiment (2026-10-30)

The local `experiment/append-effort-oct30` branch recognizes exact trusted
system/developer messages `atomic-effort:v1:<off|low|medium|high|max>` in Qwen
chat-completion requests. It renders separate prospective instructions in
place instead of hoisting a changed effort into the first system message.
Requests without these events preserve the existing template. This must be
paired with Atomic's opt-in `VITE_DSTAR_APPEND_EFFORT=true` user-turn metadata
replay. The paired experiment is now enabled in the locally installed Personal
app and Dstar executable for user testing; it has not been pushed.

In the first experiment, before the final English instruction refinement,
an isolated synthetic 2,564-token conversation was tested: the first prefill used
16.153 seconds. Later Max/Medium/Off transitions reused 3,163/3,732/3,891 tokens
and processed only 115/90/92 new tokens, taking 1.505/1.283/1.541 seconds for
prefill. Off emitted no reasoning content. The arithmetic answers were correct,
but Max did not reason longer than Low on this repeated simple problem; this
is not evidence of a general quality or reasoning-strength improvement.
Existing server/agent/web-recovery regressions passed. No model files or real
conversation were modified; the original installed server was restored Ready.

The subsequent English refinement distinguishes High from Max: High focuses on
key assumptions and relevant alternatives; Max adds counterexamples, edge cases,
failure modes, and independent checks where feasible. Low retains essential
verification. Revised server regressions passed, and the paired local deployment
reached Ready. These revised instructions have not been quality-benchmarked.

## Reasoning-only termination diagnostics

Chat completion logs now include a metadata-only `generation-diag` record with
the request diagnostic ID, stop source and token ID, client-stop flag, open
thinking state, parsed content/reasoning byte counts, tool-call count, generated
token count and limits. This distinguishes early model termination from client
stop sequences and output limits. No prompt, response text or custom stop string
is included. Generation behavior is unchanged; no automatic retry is introduced.

## Append-only tool catalog experiment (2026-10-30)

Trusted system/developer `atomic-tools:v1:<JSON array>` events render tool
catalogs at their recorded user-turn boundaries. When present, request-wide
tool definitions no longer alter the initial Qwen system prefix. Historical
calls use the parameter order from their recorded catalog, including live tool
result suffixes. Unmarked callers retain the original renderer. Current API
tools and the application's post-approval availability check remain the
execution boundary; historical catalogs do not grant authority. Existing
conversations can incur one migration prefill before later updates reuse their
prefix. All new tool result tokens still require evaluation.
