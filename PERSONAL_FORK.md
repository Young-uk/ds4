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
