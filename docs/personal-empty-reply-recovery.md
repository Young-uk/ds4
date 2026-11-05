# Empty Qwen reply recovery boundary

A Qwen Chat Completions request may emit private reasoning and terminate before
closing its thinking block. The existing one-shot recovery appends a forced
assistant prefix with thinking disabled to the live model session. Previously,
it only reset the incremental thinking flag: the append-only response buffer
still had no closing boundary. Final parsing classified the recovered answer as
reasoning, and the OpenAI stream could not switch to its content channel.

Mirror the forced closing boundary in the response buffer only when thinking is
open, feed that same boundary to the incremental tracker, and restart tool and
stop scans at the new frontier. Preserve prior streaming cursors so reasoning is
not emitted twice. Recovery instructions are not included in client output.
Private reasoning is never promoted to answer text. The model must generate a
new answer or tool call. The existing single recovery attempt, cancellation and
output-limit gates remain unchanged.

This repairs a recovery-path defect present in commit 02805726 (November 2),
not the independent-prefix caching change. It does not prove why the model
originally selected a stop token while still thinking.

Verification: server build and server tests passed, including implicit thinking,
explicit thinking, idempotent closure, final answer parsing, tool-call parsing,
and OpenAI SSE content separation with completion termination.

Installed runtime verification (November 5): installed the patched server and
retried the previously failed Winter request without adding a new user message.
It completed with 136 generated tokens, 272 content bytes, 326 reasoning bytes,
finish=stop and thinking_open=0. The installed app displayed the public answer,
separate reasoning and token statistics, with no error card. This retry closed
thinking naturally, so it validates the installed request path but does not
independently exercise the one-shot recovery branch. That branch is covered by
the parser and SSE regression tests above.

The retry cold-prefilled 54,807 tokens in 365.7 seconds because the saved failed
attempt included a recovery suffix and did not match the original request. This
is separate from the repaired output boundary. Subsequent cache reuse was not
measured in this run.

The installed production build SHA-256 is
08b60d71b4c56eace320a9b2f7d471339366b16481619ab81773a951bd821de0.
The final source adds one further tool-call regression test after that build;
production recovery behavior is unchanged.
