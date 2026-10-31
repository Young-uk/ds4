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

## Qwen disk replay trailer repair (2026-10-30)

The tool-map persistence scanner previously recognized plural DeepSeek tool
wrappers but omitted Qwen's singular `<tool_call>` syntax. KV files could be
written without the raw-call/ID trailer, leaving a fresh process unable to
reproduce the sampled prefix exactly. The scanner now uses the existing Qwen
(and GLM fallback) parser to identify complete singular-call groups, including
parameter text containing delimiter-like strings. Exact dictionary matches
preserve original leading/trailing whitespace; template-only leading whitespace
can be skipped for matching without normalizing any stored bytes. Existing
plural wrappers, model scoping, payload compatibility and prefix checks remain.

Old cache files lacking a trailer are not rewritten or trusted heuristically.
A cold replay may still be needed once to rebuild such state. Fresh-process
regressions cover single/grouped Qwen calls, whitespace, literal delimiters,
unused-ID exclusion and byte-exact replay from a disk trailer.

## Model-authored interim assessments

A trusted `atomic-tool-preamble:v1` event renders a prospective English policy
at a newly recorded turn without rewriting past catalog text. The model chooses
what interim understanding is useful to communicate and how much detail it
needs. There is no fixed phrase or sentence count and no app-generated dialogue.
The function-only tool format applies to the call block, not preceding prose.
Approval and execution rules remain unchanged. Keep deployed v1 wording
immutable; future wording changes require a new versioned event.

### Canonical Qwen final-answer disk keys

Qwen final-answer checkpoints now use the existing assistant-message renderer
for their disk replay key, including echoed reasoning and the renderer's content
whitespace rules. The saved KV payload is unchanged. This avoids falling back to
an older checkpoint when sampled whitespace differs from the next request's
rendered history. Non-Qwen behavior and exact-prefix rejection for edited
reasoning remain unchanged. Existing raw-token snapshots are not rewritten.

Regression coverage compares replay keys with the actual next-request renderer
for echoed/omitted reasoning and verifies edited reasoning is rejected. Real
model validation uses isolated cache directories and synthetic conversations;
results distinguish simple, mixed-history, leading-whitespace, and long-context
restart cases instead of treating a short successful restart as complete proof.

### Bound empty Qwen Chat Completions recovery

For Qwen OpenAI Chat Completions, a normal model stop with no non-whitespace
answer and no tool call gets one request-local continuation reminder. The model
chooses whether to answer or use an available tool; reasoning text is never
inspected to infer or execute a tool. Existing generated-token and context limits
still apply. Explicit client stop, disconnect, and shutdown never trigger this
continuation. Streaming text and cursors are retained so previous reasoning is
not replayed when the continuation starts.

If the response remains empty, or its output budget is exhausted before an
answer, return an OpenAI-compatible SSE error / HTTP 502 rather than a successful
blank answer. Other model families and API protocols keep their current behavior.
A short visible answer without a tool call is outside this empty-reply recovery.

### Dedicated background summary session

`--summary-slot` enables two resident sessions sharing model weights and supported
prefill workspaces. OpenAI chat requests with `atomic_summary: true` use the last
slot, while regular traffic stays on slot zero. The models list advertises
`atomic_summary_slot` so clients can opt in without sending summaries to a
single-session server. Foreground prefill wins the next available quantum;
already executing GPU work is not preempted and decode throughput remains
shared. Background prefill is bounded to 512 tokens per idle quantum and uses the
existing smaller mixed-prefill quantum while foreground generation is active. Summary slot
checkpoints never enter the conversation disk-cache budget.

This changes request scheduling only in the explicit summary-slot mode. Normal
single-session operation and ordinary multi-session routing remain unchanged.
Synthetic concurrent streaming at 65,536 context verified chat completion before
the summary completed, plus the app's archive/summary/restart/search state
machine with an actual model summary. Token policy and archived-source integrity
are tested separately; this does not guarantee a correct summary or zero latency.

## Bounded summary slot experiment (2026-10-31)

The opt-in `--summary-slot` experiment now allocates an 8192-token summary
session alongside the configured foreground session. Oversized summary prompts
are rejected with their actual token count and 8192 limit. Summary decode does
not use MTP and never shares a decode batch with the foreground request.
Foreground pending prefill/decode wins at the next scheduling boundary.
The paired client folds bounded evidence fragments into a summary sequentially.
Normal installed single-slot startup remains unchanged. This is not a claim
that the long-context Metal memory peak or interactive latency is production-safe.

## Tool follow-through and private cache diagnostics (2026-10-31)

Recorded v1 progress-policy events remain byte-for-byte unchanged. A new v2
user-turn event supplements them with same-turn actual tool-call follow-through,
without a fixed announcement, keyword classifier, inferred tool execution, or
new tool permissions. Direct answers remain direct. This is model guidance,
not a guarantee that an arbitrary completion will choose a tool correctly.

Tail-mismatch logs now report only approximate structural regions (system,
user, assistant, reasoning, answer, tool-call/result). No differing conversation
bytes, prompt text or reasoning content are logged. Matching and KV payloads
are unchanged. Server-unit and live behavior checks are recorded separately.
