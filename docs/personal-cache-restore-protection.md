# Protect pending disk-cache restoration

A live-slot miss used to save the outgoing conversation before looking up the
incoming conversation on disk. Under a 2 GiB budget, an unrelated 723 MiB QA
checkpoint evicted the 1,701 MiB checkpoint that the incoming request needed.
The next reboot could restore only an interrupted 5,033-token partial prefix.

The server now selects a compatible incoming text-prefix candidate while holding
its existing cache serialization and protects that path during the outgoing
save. Eviction keeps its existing ranking for every other file. If the outgoing
snapshot cannot fit alongside the protected candidate, it skips that save instead
of evicting the requested checkpoint or exceeding the configured budget. The
protection is cleared before the ordinary loader validates and restores the file.
No file format, cache budget, model settings, or inference algorithm changes.

Synthetic regression files use a dedicated mkdtemp directory and never the user's
cache. Model-based QA must likewise use a separate server/cache directory; do not
send synthetic conversations into the production endpoint merely to test caching.
This change does not identify synthetic requests by prompt text or change how
unrelated idle conversations compete outside a pending restoration.

Validation: server unit suite, including competing-file and tight-budget restore
protection; release server build. Live restart results are recorded separately.
